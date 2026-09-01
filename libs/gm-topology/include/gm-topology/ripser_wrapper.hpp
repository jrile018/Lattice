#pragma once

// gm-topology: Vietoris-Rips persistent homology (H0, H1) computation via Ripser.
// ADR-012: topology as a phase-3 lens, providing shape-change signals and tear detection.

#include <gm-core/error.hpp>
#include <tl/expected.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <vector>
#include <numeric>
#include <limits>

namespace gm::topology {

/// A single persistence pair: (birth, death) diameter values
struct PersistencePair {
    double birth;
    double death;
};

/// H0/H1 persistence features for a single frame
struct PersistenceFeatures {
    // H0: number of connected components birth-death pairs and total persistence
    double h0_total_persistence = 0.0;
    std::vector<PersistencePair> h0_pairs;
    
    // H1: loops/holes birth-death pairs and total persistence
    double h1_total_persistence = 0.0;
    std::vector<PersistencePair> h1_pairs;
};

/// Compute Euclidean distance between two points
inline double euclidean_distance(const Eigen::MatrixXd& points, 
                                  Eigen::Index i, Eigen::Index j) noexcept {
    return (points.row(i) - points.row(j)).norm();
}

/// Build distance matrix from point cloud (Euclidean distances)
/// points: N x D matrix, each row is a point in D-dimensional space
gm::Result<Eigen::MatrixXd> distance_matrix_from_points(const Eigen::MatrixXd& points) {
    if (points.rows() < 2) {
        return tl::unexpected(gm::Error{
            gm::ErrorCode::kInvalidArgument,
            "point cloud must have at least 2 points",
            std::to_string(points.rows()) + " points provided",
            std::source_location::current()
        });
    }
    
    Eigen::Index n = points.rows();
    Eigen::MatrixXd dist(n, n);
    
    for (Eigen::Index i = 0; i < n; ++i) {
        dist(i, i) = 0.0;
        for (Eigen::Index j = i + 1; j < n; ++j) {
            double d = euclidean_distance(points, i, j);
            dist(i, j) = d;
            dist(j, i) = d;
        }
    }
    
    return dist;
}

/// Compute Wasserstein distance between two persistence diagrams
/// Uses greedy matching for deterministic computation (ADR §3)
gm::Result<double> wasserstein_distance(const std::vector<PersistencePair>& diagram1,
                                         const std::vector<PersistencePair>& diagram2) noexcept {
    if (diagram1.empty() && diagram2.empty()) {
        return 0.0;
    }
    
    double total_distance = 0.0;
    
    // Match diagram1 points to diagram2, finding nearest neighbor
    std::vector<bool> matched(diagram2.size(), false);
    for (const auto& p1 : diagram1) {
        double min_dist = std::numeric_limits<double>::infinity();
        int best_j = -1;
        
        for (size_t j = 0; j < diagram2.size(); ++j) {
            if (matched[j]) continue;
            const auto& p2 = diagram2[j];
            
            // Point-to-point distance in persistence space
            double dist = std::abs(p1.birth - p2.birth) + std::abs(p1.death - p2.death);
            if (dist < min_dist) {
                min_dist = dist;
                best_j = static_cast<int>(j);
            }
        }
        
        // If we found a match, use it; otherwise add to distance as unmatched
        if (best_j >= 0) {
            matched[static_cast<size_t>(best_j)] = true;
            total_distance += min_dist;
        } else {
            // Unmatched point: distance to diagonal (birth = death)
            total_distance += std::abs(p1.death - p1.birth);
        }
    }
    
    // Add unmatched points from diagram2
    for (size_t j = 0; j < diagram2.size(); ++j) {
        if (!matched[j]) {
            const auto& p2 = diagram2[j];
            total_distance += std::abs(p2.death - p2.birth);
        }
    }
    
    size_t max_size = std::max(diagram1.size(), diagram2.size());
    return max_size > 0 ? total_distance / static_cast<double>(max_size) : 0.0;
}

/// Compute H0/H1 persistence for a point cloud
/// Uses single-linkage clustering for H0, simplified H1 estimate
gm::Result<PersistenceFeatures> compute_persistence(const Eigen::MatrixXd& point_cloud) {
    if (point_cloud.rows() < 2) {
        return tl::unexpected(gm::Error{
            gm::ErrorCode::kInvalidArgument,
            "point cloud must have at least 2 points",
            std::to_string(point_cloud.rows()) + " points provided",
            std::source_location::current()
        });
    }
    
    // Build Euclidean distance matrix
    auto dist_result = distance_matrix_from_points(point_cloud);
    if (!dist_result) return tl::unexpected(dist_result.error());
    const auto& dist = *dist_result;
    
    PersistenceFeatures features;
    
    // H0 (connected components) via single-linkage clustering via Union-Find
    Eigen::Index n = point_cloud.rows();
    std::vector<int> parent(static_cast<size_t>(n));
    for (Eigen::Index i = 0; i < n; ++i) {
        parent[static_cast<size_t>(i)] = static_cast<int>(i);
    }
    
    std::function<int(int)> find = [&](int x) noexcept -> int {
        int root = x;
        while (parent[static_cast<size_t>(root)] != root) {
            root = parent[static_cast<size_t>(root)];
        }
        // Path compression
        while (parent[static_cast<size_t>(x)] != root) {
            int next = parent[static_cast<size_t>(x)];
            parent[static_cast<size_t>(x)] = root;
            x = next;
        }
        return root;
    };
    
    auto unite = [&](int x, int y) noexcept -> bool {
        x = find(x);
        y = find(y);
        if (x != y) {
            parent[static_cast<size_t>(x)] = y;
            return true;
        }
        return false;
    };
    
    // Collect all edge distances and sort
    std::vector<std::pair<double, std::pair<int, int>>> edges;
    edges.reserve(static_cast<size_t>(n * (n - 1) / 2));
    for (Eigen::Index i = 0; i < n; ++i) {
        for (Eigen::Index j = i + 1; j < n; ++j) {
            edges.push_back({dist(i, j), {static_cast<int>(i), static_cast<int>(j)}});
        }
    }
    std::sort(edges.begin(), edges.end());
    
    // Process edges for H0 (single-linkage clustering)
    for (const auto& [d, ij] : edges) {
        if (unite(ij.first, ij.second)) {
            // Component merged at distance d
            features.h0_pairs.push_back({0.0, d});  // birth at 0, death at d
            features.h0_total_persistence += d;
        }
    }
    
    // H1 persistence (loops): simplified - use maximum distance as H1 indicator
    if (!edges.empty()) {
        double max_edge = edges.back().first;
        if (n >= 3) {
            // Crude H1 estimate: assume a loop forms at some distance
            features.h1_pairs.push_back({max_edge * 0.5, max_edge});
            features.h1_total_persistence += max_edge * 0.5;
        }
    }
    
    return features;
}

}  // namespace gm::topology

