#pragma once

#include <optional>

#include "../common.h"

#include "polynomials.h"

#include <carl-arith/poly/umvpoly/functions/Factorization.h>
#include <carl-arith/poly/umvpoly/functions/Resultant.h>

namespace smtrat::cadcells::datastructures {

namespace detail {

struct PolyProperties {
    std::map<PolyRef, PolyRef> res;
    std::optional<PolyRef> disc;
    std::optional<PolyRef> ldcf;
    std::vector<PolyRef> factors_nonconst;
};

struct AssignmentProperties {
    std::map<PolyRef, carl::ran::RealRootsResult<RAN>> real_roots;
    std::map<PolyRef, bool> is_zero;
};

}

/**
 * Encapsulates all computations on polynomials. 
 * Computations are cached with respect to a PolyPool.
 */
class Projections {
    PolyPool& m_pool;
    std::vector<std::vector<detail::PolyProperties>> m_poly_cache;
    std::vector<std::map<Assignment,detail::AssignmentProperties>> m_assignment_cache;

    auto& cache(PolyRef p) {
        assert(p.level > 0);
        m_poly_cache.resize(p.level);
        m_poly_cache[p.level-1].resize(p.id+1);
        return m_poly_cache[p.level-1][p.id];
    }
    const auto& cache(PolyRef p) const {
        assert(p.level > 0);
        assert(p.level-1 < m_poly_cache.size());
        assert(p.id < m_poly_cache[p.level-1].size());
        return m_poly_cache[p.level-1][p.id];
    }

    size_t level_of(const Assignment& a) const {
        return a.size();
    }

    auto& cache(const Assignment& a) {
        m_assignment_cache.resize(level_of(a)+1);
        return m_assignment_cache[level_of(a)][a];
    }
    const auto& cache(const Assignment& a) const {
        assert(level_of(a) < m_assignment_cache.size());
        assert(m_assignment_cache[level_of(a)].find(a) != m_assignment_cache[level_of(a)].end());
        return m_assignment_cache[level_of(a)].at(a);
    }

public:
    auto main_var(PolyRef p) const {
        return m_pool.var_order()[p.level-1];
    }

private:
    auto as_univariate(PolyRef p) const {
        // TODO @Philip: for LPPoly, just return the polynomial itself here 
        return carl::to_univariate_polynomial(m_pool(p), main_var(p));
    }

    Assignment restrict_assignment(Assignment ass, PolyRef p) {
        auto vars = carl::variables(m_pool(p));
        for(auto i = m_pool.var_order().rbegin(); i !=  m_pool.var_order().rend(); i++) {
            if (!vars.has(*i)) ass.erase(*i);
            else return ass;
        }
        return ass;
    }

public:
    Projections(PolyPool& pool) : m_pool(pool) {}

    auto& polys() { return m_pool; }
    const auto& polys() const { return m_pool; }

    /// Clears all polynomials of the specified level and higher in the polynomial cache as well as their projection results.
    void clear_cache(size_t level) {
        assert(level > 0);
        m_pool.clear_levels(level);
        if (level <= m_poly_cache.size()) {
            m_poly_cache.erase(m_poly_cache.begin() + (level - 1), m_poly_cache.end());
        }
        if (level < m_assignment_cache.size()) {
            m_assignment_cache.erase(m_assignment_cache.begin() + level, m_assignment_cache.end());
        }
    }

    /// Clears all projections cached with respect to this assignment.
    void clear_assignment_cache(const Assignment& assignment) {
        if (level_of(assignment) < m_assignment_cache.size()) {
            m_assignment_cache[level_of(assignment)].erase(assignment);
        }
    }
    
    PolyRef res(PolyRef p, PolyRef q) {
        assert(p.level == q.level && p.id != q.id);

        if (p.id > q.id) return res(q,p);
        assert(p.id < q.id);

        if (cache(p).res.find(q) != cache(p).res.end()) {
            return cache(p).res[q];
        } else {
            auto upoly = carl::resultant(as_univariate(p), as_univariate(q));
            assert(carl::is_constant(upoly));
            auto result = m_pool(Poly(upoly));
            assert(!is_zero(result));
            cache(p).res.emplace(q, result);
            return result;
        }
    }

    bool know_disc(PolyRef p) const {
        return (bool) cache(p).disc;
    }

    bool known(const Polynomial& p) const {
        return m_pool.known(p);
    } 

    PolyRef disc(PolyRef p) {
        if (cache(p).disc) {
            return *cache(p).disc;
        } else {
            auto upoly = carl::discriminant(as_univariate(p));
            assert(carl::is_constant(upoly));
            auto result = m_pool(Poly(upoly));
            assert(!is_zero(result));
            cache(p).disc = result;
            return result;
        }
    }

    PolyRef ldcf(PolyRef p) {
        if (cache(p).ldcf) {
            return *cache(p).ldcf;
        } else {
            auto result = m_pool(m_pool(p).lcoeff(main_var(p)));
            assert(!is_zero(result));
            cache(p).ldcf = result;
            return result;
        }
    }

    const std::vector<PolyRef>& factors_nonconst(PolyRef p) {
        if (cache(p).factors_nonconst.empty()) {
            for (const auto& factor : carl::irreducible_factors(m_pool(p), false)) {
                cache(p).factors_nonconst.emplace_back(m_pool(factor));
            }
        }
        return cache(p).factors_nonconst;
    }

    bool is_zero(const Assignment& sample, PolyRef p) {
        auto restricted_sample = restrict_assignment(sample ,p);
        assert(p.level == level_of(restricted_sample));
        if (restricted_sample.empty()) return is_zero(p);
        if (cache(restricted_sample).is_zero.find(p) == cache(restricted_sample).is_zero.end()) {
            auto mv = carl::evaluate(carl::BasicConstraint<Polynomial>(m_pool(p), carl::Relation::EQ), restricted_sample);
            assert(!indeterminate(mv));
            cache(restricted_sample).is_zero[p] = (bool) mv;
        }
        return cache(restricted_sample).is_zero[p];
    }

    size_t num_roots(const Assignment& sample, PolyRef p) {
        assert(p.level == level_of(sample)+1);
        assert(!m_pool(p).is_constant());
        if (cache(sample).real_roots.find(p) == cache(sample).real_roots.end()) {
            cache(sample).real_roots.emplace(p, carl::real_roots(as_univariate(p), sample));
        }
        assert(cache(sample).real_roots.at(p).is_univariate());
        return cache(sample).real_roots.at(p).roots().size();
    }

    std::vector<RAN> real_roots(const Assignment& sample, PolyRef p) {
        assert(p.level == level_of(sample)+1);
        assert(!m_pool(p).is_constant());
        if (cache(sample).real_roots.find(p) == cache(sample).real_roots.end()) {
            cache(sample).real_roots.emplace(p, carl::real_roots(as_univariate(p), sample));
        }
        assert(cache(sample).real_roots.at(p).is_univariate());
        return cache(sample).real_roots.at(p).roots();
    }

    bool is_nullified(const Assignment& sample, PolyRef p) {
        assert(p.level == level_of(sample)+1);
        auto poly = m_pool(p);
		assert(!poly.is_constant());
		if (poly.is_linear()) return false;
        if (cache(sample).real_roots.find(p) == cache(sample).real_roots.end()) {
            cache(sample).real_roots.emplace(p, carl::real_roots(as_univariate(p), sample));
        }
		return cache(sample).real_roots.at(p).is_nullified();
    }

    bool is_ldcf_zero(const Assignment& sample, PolyRef p) {
        return is_zero(sample, ldcf(p));
    }

    bool is_disc_zero(const Assignment& sample, PolyRef p) {
        return is_zero(sample, disc(p));
    }

    bool is_const(PolyRef p) {
        return carl::is_constant(m_pool(p));
    }

    bool is_zero(PolyRef p) {
        return carl::is_zero(m_pool(p));
    }

    bool has_const_coeff(PolyRef p) const {
        auto poly = as_univariate(p);
        for (const auto& coeff :  poly.coefficients()) {
            if (coeff.is_constant() && !carl::is_zero(coeff)) return true;
        }
        return false;
    }

    PolyRef simplest_nonzero_coeff(const Assignment& sample, PolyRef p, std::function<bool(const Polynomial&,const Polynomial&)> compare) const {
        std::optional<Polynomial> result;
        auto poly = as_univariate(p);
        for (const auto& coeff : poly.coefficients()) {
            auto mv = carl::evaluate(carl::BasicConstraint<Polynomial>(coeff, carl::Relation::NEQ), sample);
            assert(!indeterminate(mv));
            if (mv) {
                if (!result || compare(coeff,*result)) {
                    result = coeff;
                }
            }
        }
        assert(result);
        return m_pool(*result);
    }

    std::size_t degree(PolyRef p) {
        return m_pool(p).degree(main_var(p));
    }

    std::size_t max_degree(PolyRef p) {
        const auto& poly = m_pool(p);
        size_t deg = 0;
        for (const auto var : carl::variables(poly)) {
            deg = std::max(deg, poly.degree(var));
        }
        return deg;
    }

};

}