/*
    SPDX-FileCopyrightText: Milian Wolff <milian.wolff@kdab.com>
    SPDX-FileCopyrightText: 2016-2022 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "data.h"

#include <QDebug>
#include <QSet>

using namespace Data;

namespace {

ItemCost buildTopDownResult(const BottomUp& bottomUpData, const Costs& bottomUpCosts, TopDown* topDownData,
                            Costs* inclusiveCosts, Costs* selfCosts, quint32* maxId, bool skipFirstLevel)
{
    ItemCost totalCost;
    totalCost.resize(bottomUpCosts.numTypes(), 0);
    for (const auto& row : bottomUpData.children) {
        // recurse and find the cost attributed to children
        const auto childCost =
            buildTopDownResult(row, bottomUpCosts, topDownData, inclusiveCosts, selfCosts, maxId, skipFirstLevel);
        const auto rowCost = bottomUpCosts.itemCost(row.id);
        const auto diff = rowCost - childCost;
        if (diff.sum() != 0) {
            // this row is (partially) a leaf
            // bubble up the parent chain to build a top-down tree
            auto node = &row;
            auto stack = topDownData;
            while (node) {
                auto frame = stack->entryForSymbol(node->symbol, maxId);

                const auto isLastNode = !node->parent || (skipFirstLevel && !node->parent->parent);

                // always use the leaf node's cost and propagate that one up the chain
                // otherwise we'd count the cost of some nodes multiple times
                inclusiveCosts->add(frame->id, diff);
                if (isLastNode) {
                    selfCosts->add(frame->id, diff);
                    break;
                }

                stack = frame;
                node = node->parent;
            }
        }
        totalCost += rowCost;
    }
    return totalCost;
}

void add(ItemCost& lhs, const ItemCost& rhs)
{
    if (!lhs.size()) {
        lhs = rhs;
    } else {
        Q_ASSERT(lhs.size() == rhs.size());
        lhs += rhs;
    }
}

ItemCost buildCallerCalleeResult(const BottomUp& data, const Costs& bottomUpCosts, CallerCalleeResults* results)
{
    ItemCost totalCost;
    totalCost.resize(bottomUpCosts.numTypes(), 0);
    for (const auto& row : data.children) {
        // recurse to find a leaf
        const auto childCost = buildCallerCalleeResult(row, bottomUpCosts, results);
        const auto rowCost = bottomUpCosts.itemCost(row.id);
        const auto diff = rowCost - childCost;
        if (diff.sum() != 0) {
            // this row is (partially) a leaf

            // leaf node found, bubble up the parent chain to add cost for all frames
            // to the caller/callee data. this is done top-down since we must not count
            // symbols more than once in the caller-callee data
            QSet<Symbol> recursionGuard;
            auto node = &row;

            QSet<QPair<Symbol, Symbol>> callerCalleeRecursionGuard;
            Data::Symbol lastSymbol;
            Data::CallerCalleeEntry* lastEntry = nullptr;

            while (node) {
                const auto& symbol = node->symbol;
                // aggregate caller-callee data
                auto& entry = results->entry(symbol);

                if (!recursionGuard.contains(symbol)) {
                    // only increment inclusive cost once for a given stack
                    results->inclusiveCosts.add(entry.id, diff);
                    recursionGuard.insert(symbol);
                }
                if (!node->parent) {
                    // always increment the self cost
                    results->selfCosts.add(entry.id, diff);
                }
                // add current entry as callee to last entry
                // and last entry as caller to current entry
                if (lastEntry) {
                    const auto callerCalleePair = qMakePair(symbol, lastSymbol);
                    if (!callerCalleeRecursionGuard.contains(callerCalleePair)) {
                        add(lastEntry->callee(symbol, bottomUpCosts.numTypes()), diff);
                        add(entry.caller(lastSymbol, bottomUpCosts.numTypes()), diff);
                        callerCalleeRecursionGuard.insert(callerCalleePair);
                    }
                }

                node = node->parent;
                lastSymbol = symbol;
                lastEntry = &entry;
            }
        }
        totalCost += rowCost;
    }
    return totalCost;
}

int findSameDepth(QStringView str, int offset, QChar ch, bool returnNext = false)
{
    const int size = str.size();

    int depth = 0;
    for (; offset < size; ++offset) {
        const auto current = str[offset];
        if (current == QLatin1Char('<') || current == QLatin1Char('(')) {
            ++depth;
        } else if (current == QLatin1Char('>') || current == QLatin1Char(')')) {
            --depth;
        }

        if (depth == 0 && current == ch) {
            return offset + (returnNext ? 1 : 0);
        }
    }
    return -1;
}

template<typename T>
int startsWith(QStringView str, const std::initializer_list<T>& prefixes)
{
    for (const auto& prefix_ : prefixes) {
        const auto prefix = QLatin1String(prefix_);
        if (str.startsWith(prefix)) {
            return prefix.size();
        }
    }
    return -1;
}

QString prettifySymbol(QStringView str)
{
    int pos = 0;
    do {
        pos = str.indexOf(QLatin1String("std::"), pos);
        if (pos == -1) {
            return str.toString();
        }
        pos += 5;
        if (pos == 5 || str[pos - 6] == QLatin1Char('<') || str[pos - 6] == QLatin1Char(' ')
            || str[pos - 6] == QLatin1Char('(')) {
            break;
        }
    } while (true);

    auto result = str.left(pos).toString();
    auto symbol = str.mid(pos);

    int end;

    if ((end = startsWith(symbol, {"__cxx11::", "__1::"})) != -1) {
        // Strip libstdc++/libc++ internal namespace
        symbol = symbol.mid(end);
    }

    const auto oneParameterTemplates = {"vector<",       "set<",      "deque<",         "list<",
                                        "forward_list<", "multiset<", "unordered_set<", "unordered_multiset<"};
    const auto twoParametersTemplates = {"map<", "multimap<", "unordered_map<", "unordered_multimap<"};

    // Translate basic_string<(char|wchar_t|T), ...> to (string|wstring|basic_string<T>)
    if ((end = startsWith(symbol, {"basic_string<"})) != -1) {
        const int comma = findSameDepth(symbol, end, QLatin1Char(','));
        if (comma != -1) {
            const auto type = symbol.mid(end, comma - end);
            if (type == QLatin1String("char")) {
                result += QLatin1String("string");
            } else if (type == QLatin1String("wchar_t")) {
                result += QLatin1String("wstring");
            } else {
                result += symbol.left(end);
                result += type;
                result += QLatin1Char('>');
            }
            end = findSameDepth(symbol, 0, QLatin1Char('>'), true);
            symbol = symbol.mid(end);

            // Also translate constructor/destructor name
            const auto constructorDestructor = {"::basic_string(", "::~basic_string("};
            if ((end = startsWith(symbol, constructorDestructor)) != -1) {
                result += QLatin1String("::");
                if (symbol[2] == QLatin1Char('~')) {
                    result += QLatin1Char('~');
                }
                if (type == QLatin1String("wchar_t")) {
                    result += QLatin1Char('w');
                } else if (type != QLatin1String("char")) {
                    result += QLatin1String("basic_");
                }
                result += QLatin1String("string(");
                symbol = symbol.mid(end);
            }
        }
    }
    // Translates (vector|set|etc.)<T, ...> to (vector|set|etc.)<T>
    else if ((end = startsWith(symbol, oneParameterTemplates)) != -1) {
        const int comma = findSameDepth(symbol, end, QLatin1Char(','));
        if (comma != -1) {
            result += symbol.left(end);
            result += prettifySymbol(symbol.mid(end, comma - end));
            result += QLatin1Char('>');

            end = findSameDepth(symbol, 0, QLatin1Char('>'), true);
            symbol = symbol.mid(end);
        }
    }
    // Translates (map|multimap|etc.)<T, U, ...> to (map|multimap|etc.)<T, U>
    else if ((end = startsWith(symbol, twoParametersTemplates)) != -1) {
        const int comma1 = findSameDepth(symbol, end, QLatin1Char(','));
        const int comma2 = findSameDepth(symbol, comma1 + 1, QLatin1Char(','));
        if (comma1 != -1 && comma2 != -1) {
            result += symbol.left(end);
            result += prettifySymbol(symbol.mid(end, comma1 - end));
            result += prettifySymbol(symbol.mid(comma1, comma2 - comma1));
            result += QLatin1Char('>');

            end = findSameDepth(symbol, 0, QLatin1Char('>'), true);
            symbol = symbol.mid(end);
        }
    }
    // Translates allocator<T> to allocator<...>
    else if ((end = startsWith(symbol, {"allocator<"})) != -1) {
        const int gt = findSameDepth(symbol, 0, QLatin1Char('>'), true);
        if (gt != -1) {
            result += symbol.left(end);
            result += QLatin1String("...>");

            symbol = symbol.mid(gt);
        }
    }

    if (!symbol.isEmpty()) {
        result += prettifySymbol(symbol);
    }

    return result;
}

void buildPerLibrary(const TopDown* node, PerLibraryResults& results, QHash<QString, int>& binaryToResultIndex,
                     const Costs& costs)
{
    for (const auto& child : node->children) {
        const auto binary = child.symbol.binary;

        auto resultIndexIt = binaryToResultIndex.find(binary);
        if (resultIndexIt == binaryToResultIndex.end()) {
            resultIndexIt = binaryToResultIndex.insert(binary, binaryToResultIndex.size());

            PerLibrary library;
            library.id = *resultIndexIt;
            library.symbol = Symbol(binary);
            results.root.children.push_back(library);
        }

        const auto cost = costs.itemCost(child.id);
        results.costs.add(*resultIndexIt, cost);

        buildPerLibrary(&child, results, binaryToResultIndex, costs);
    }
}
}

QString Data::prettifySymbol(const QString& name)
{
    const auto result = ::prettifySymbol(QStringView(name));
    return result == name ? name : result;
}

TopDownResults TopDownResults::fromBottomUp(const BottomUpResults& bottomUpData, bool skipFirstLevel)
{
    TopDownResults results;
    results.selfCosts.initializeCostsFrom(bottomUpData.costs);
    results.inclusiveCosts.initializeCostsFrom(bottomUpData.costs);
    quint32 maxId = 0;
    if (skipFirstLevel) {
        results.root.children.reserve(bottomUpData.root.children.size());
        for (const auto& bottomUpGroup : bottomUpData.root.children) {
            // manually copy the first level
            auto topDownGroup = results.root.entryForSymbol(bottomUpGroup.symbol, &maxId);
            // then traverse the children as separate trees basically
            buildTopDownResult(bottomUpGroup, bottomUpData.costs, topDownGroup, &results.inclusiveCosts,
                               &results.selfCosts, &maxId, true);
            // finally manually sum up the inclusive costs
            for (const auto& child : std::as_const(topDownGroup->children)) {
                results.inclusiveCosts.add(topDownGroup->id, results.inclusiveCosts.itemCost(child.id));
            }
        }
    } else {
        buildTopDownResult(bottomUpData.root, bottomUpData.costs, &results.root, &results.inclusiveCosts,
                           &results.selfCosts, &maxId, false);
    }
    TopDown::initializeParents(&results.root);
    return results;
}

PerLibraryResults PerLibraryResults::fromTopDown(const TopDownResults& topDownData)
{
    PerLibraryResults results;
    QHash<QString, int> binaryToResultIndex;
    results.costs.initializeCostsFrom(topDownData.selfCosts);

    buildPerLibrary(&topDownData.root, results, binaryToResultIndex, topDownData.selfCosts);

    PerLibrary::initializeParents(&results.root);

    return results;
}

void Data::callerCalleesFromBottomUpData(const BottomUpResults& bottomUpData, CallerCalleeResults* results)
{
    results->inclusiveCosts.initializeCostsFrom(bottomUpData.costs);
    results->selfCosts.initializeCostsFrom(bottomUpData.costs);
    buildCallerCalleeResult(bottomUpData.root, bottomUpData.costs, results);
}

QDebug Data::operator<<(QDebug stream, const Symbol& symbol)
{
    stream.noquote().nospace() << "Symbol{"
                               << "symbol=" << symbol.symbol << ", "
                               << "relAddr=" << symbol.relAddr << ", "
                               << "size=" << symbol.size << ", "
                               << "binary=" << symbol.binary << "}";
    return stream.resetFormat().space();
}

QDebug Data::operator<<(QDebug stream, const FileLine& fileLine)
{
    stream.noquote().nospace() << "FileLine{"
                               << "file=" << fileLine.file << ", "
                               << "line=" << fileLine.line << "}";
    return stream.resetFormat().space();
}

QDebug Data::operator<<(QDebug stream, const Location& location)
{
    stream.noquote().nospace() << "Location{"
                               << "address=" << location.address << ", "
                               << "relAddr=" << location.relAddr << ", "
                               << "fileLine=" << location.fileLine << "}";
    return stream.resetFormat().space();
}

QDebug Data::operator<<(QDebug stream, const ItemCost& cost)
{
    stream.noquote().nospace() << "ItemCost(" << cost.size() << "){";
    for (auto c : cost) {
        stream << c << ",";
    }
    stream << "}";
    return stream.resetFormat().space();
}

QDebug Data::operator<<(QDebug stream, const CostSummary& cost)
{
    stream.noquote().nospace() << "CostSummary{"
                               << "label = " << cost.label << ", "
                               << "sampleCount = " << cost.sampleCount << ", "
                               << "totalPeriod = " << cost.totalPeriod << "}";
    return stream.resetFormat().space();
}

Data::ThreadEvents* Data::EventResults::findThread(qint32 pid, qint32 tid)
{
    for (int i = threads.size() - 1; i >= 0; --i) {
        auto& thread = threads[i];
        if (thread.pid == pid && thread.tid == tid) {
            return &thread;
        }
    }

    return nullptr;
}

const Data::ThreadEvents* Data::EventResults::findThread(qint32 pid, qint32 tid) const
{
    return const_cast<Data::EventResults*>(this)->findThread(pid, tid);
}
