#pragma once

#include "../common.h"

#include "polynomials.h"
#include "properties.h"
#include "projections.h"
#include "delineation.h"

namespace smtrat::cadcells::datastructures {

template<typename Properties>
class base_derivation;

template<typename Properties>
class sampled_derivation;

template<typename Properties>
using base_derivation_ref = std::shared_ptr<base_derivation<Properties>>;

template<typename Properties>
using sampled_derivation_ref = std::shared_ptr<sampled_derivation<Properties>>;

template<typename Properties>
using derivation_ref = std::variant<base_derivation_ref<Properties>, sampled_derivation_ref<Properties>>;

template<typename Properties>
base_derivation_ref<Properties> base_of(derivation_ref<Properties> derivation) {
    if (std::holds_alternative<base_derivation_ref<Properties>>(derivation)) {
        return std::get<base_derivation_ref<Properties>>(derivation);
    } else {
        return std::get<sampled_derivation_ref<Properties>>(derivation)->base();
    }
}

template<typename Properties>
class base_derivation {
    projections& m_projections;

    size_t m_level;
    Properties m_properties;
    delineation m_delineation;

    derivation_ref<Properties> m_underlying; 

    base_derivation(const projections& projections, size_t level, derivation_ref<Properties> underlying) : m_projections(projections), m_level(level), m_underlying(underlying) {
        assert(level == 0 && underlying == std::nullptr || level > 0 && underlying != std::nullptr);
    }

    friend derivation_ref<Properties> make_derivation(projections& projections, const assignment& assignment);

public:

    auto& polys() { return m_projections.polys(); }
    auto& proj() { return m_projections; }
    auto main_var() {
        if (m_level == 0) return carl::Variable::NO_VARIABLE;
        else return polys().var_order()[m_level-1];
    }
    size_t level() { return m_level; }

    auto underlying() { assert(m_level > 0); return m_underlying; }
    auto underlying_cell() { assert(m_level > 0); return std::get<sampled_derivation_ref<Properties>>(m_underlying); }
    assignment& underlying_sample() { assert(m_level > 0); return underlying_cell()->sample(); }

    auto& delin() { return m_delineation; }

    template<typename P>
    void insert(P&& property) {
        assert(property.level() <= m_level && property.level() > 0);

        if (property.level() == m_level) {
            get<P>(m_properties).emplace(std::move(property));
        } else {
            assert(m_underlying != nullptr);
            m_underlying->insert(std::move(property));
        }
    }

    template<typename P>
    bool contains(const P& property) {
        assert(property.level() <= m_level && property.level() > 0);

        if (property.level() == m_level) {
            return get<P>(m_properties).contains(property);
        } else {
            assert(m_underlying != nullptr);
            return m_underlying->contains(property);
        }
    }

    template<typename P>
    const std::set<P> properties() {
        return get<P>(m_properties);
    }

    void merge(const base_derivation<Properties>& other) {
        assert(other.m_level == m_level && &other.m_projections == &m_projections);
        assert(m_delineation.empty() && other.m_delineation.empty());
        merge(m_properties, other.m_properties);
        if (m_level > 0) {
            base_of(m_underlying)->merge(base_of(*other.m_underlying));
        }
    }
};

template<typename Properties>
class sampled_derivation {
    base_derivation_ref<Properties> m_base;
    std::optional<delineation_cell> m_cell;
    assignment m_sample;

    sampled_derivation(base_derivation_ref<Properties> base, ran main_sample) : m_base(base) {
        m_sample = base().underlying_sample();
        m_sample.insert(base().main_var(), main_sample);
    }

    friend derivation_ref<Properties> make_derivation(projections& projections, const assignment& assignment);
    friend class base_derivation<Properties>;

public:
    auto& proj() { return m_base.proj(); }
    auto& base() { return m_base; }
    const delineation_cell& cell() { return *m_cell; }

    void delineate_cell() {
        m_cell = base().delin().delineate_cell(m_sample(base().main_var()));
    }

    const assignment& sample() {
        return m_sample;
    }
};

template<typename Properties>
derivation_ref<Properties> make_derivation(projections& proj, const assignment& assignment, size_t level) {
    const auto& vars = proj.polys().var_order();

    derivation_ref<Properties> current = std::make_shared<base_derivation<Properties>>(proj, 0, nullptr);
    for (size_t i = 1; i <= level; i++) {
        if (assignment.find(vars[level-1]) != assignment.end()) {
            auto base = std::make_shared<base_derivation<Properties>>(proj, level, current);
            current = std::make_shared<sampled_derivation<Properties>>(base, assignment.at(vars[level-1]));
        } else {
            current = std::make_shared<base_derivation<Properties>>(proj, level, current);
        }
    }

    return current;
}

template<typename Properties>
sampled_derivation_ref<Properties> make_sampled_derivation(base_derivation_ref<Properties> delineation, const ran& main_sample) {
    assert(std::holds_alternative<sampled_derivation_ref<Properties>>(delineation->m_underlying));
    auto cell_del = std::make_shared<sampled_derivation<Properties>>(delineation, main_sample);
    cell_del.delineate_cell();
    return cell_del;
}

template<typename Properties>
void merge_underlying(std::vector<derivation_ref<Properties>>& derivations) {
    std::set<derivation_ref<Properties>> underlying;
    for (const auto& deriv : derivations) {
        underlying.insert(base_of(base_of(deriv).underlying()));
    }
    assert(!underlying.empty());
    for (auto iter = underlying.begin()+1; iter != underlying.end(); iter++) {
        underlying.front().merge(*iter);
    }
    for (const auto& deriv : derivations) {
        base_of(deriv).m_underlying = underlying.front();
    }
}


}