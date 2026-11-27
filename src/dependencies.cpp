#include <clab/evaluator.hpp>

#include <map>
#include <set>

namespace clab {
std::vector<std::string> resolve_dependencies(std::span<const DependencySpec> graph,
                                              std::span<const std::string_view> requested) {
    std::map<std::string_view, std::size_t> indices;
    for (std::size_t i = 0; i < graph.size(); ++i) {
        if (graph[i].id.empty() || !indices.emplace(graph[i].id, i).second) {
            throw Error(ErrorCode::invalid, "empty or duplicate dependency ID: " +
                                            std::string(graph[i].id));
        }
    }
    const auto index_of = [&](std::string_view id) {
        const auto found = indices.find(id);
        if (found == indices.end()) {
            throw Error(ErrorCode::invalid, "unknown dependency ID: " + std::string(id));
        }
        return found->second;
    };

    std::vector<std::size_t> prerequisite_count(graph.size());
    std::vector<std::vector<std::size_t>> parents(graph.size()), dependents(graph.size());
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < graph.size(); ++i) {
        std::set<std::size_t> seen;
        for (auto id : graph[i].prerequisites) {
            const auto parent = index_of(id);
            if (!seen.insert(parent).second) {
                throw Error(ErrorCode::invalid, "duplicate prerequisite for: " + std::string(graph[i].id));
            }
            parents[i].push_back(parent);
            dependents[parent].push_back(i);
        }
        prerequisite_count[i] = parents[i].size();
        if (parents[i].empty()) {
            ready.push_back(i);
        }
    }

    std::vector<bool> required(graph.size(), requested.empty());
    std::vector<std::size_t> pending;
    for (const auto id : requested) {
        pending.push_back(index_of(id));
    }
    while (!pending.empty()) {
        const auto i = pending.back();
        pending.pop_back();
        if (!required[i]) {
            required[i] = true;
            pending.insert(pending.end(), parents[i].begin(), parents[i].end());
        }
    }

    std::vector<std::string> order;
    for (std::size_t position = 0; position < ready.size(); ++position) {
        const auto i = ready[position];
        if (required[i]) {
            order.emplace_back(graph[i].id);
        }
        for (const auto dependent : dependents[i]) {
            if (--prerequisite_count[dependent] == 0) {
                ready.push_back(dependent);
            }
        }
    }
    if (ready.size() != graph.size()) {
        throw Error(ErrorCode::invalid, "dependency graph contains a cycle");
    }
    return order;
}
}
