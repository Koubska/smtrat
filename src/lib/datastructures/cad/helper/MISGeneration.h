#pragma once

#include "ConflictGraph.h"
#include "../Common.h"

namespace smtrat {
namespace cad {
	
	template<MISHeuristic heuristic>
	class MISGeneration {
	public:
		template<typename CAD>
		void operator()(const CAD& cad, std::vector<FormulaSetT>& mis);
	};
	
	template<>
	template<typename CAD>
	void MISGeneration<MISHeuristic::TRIVIAL>::operator()(const CAD& cad, std::vector<FormulaSetT>& mis) {
		static int x;
		SMTRAT_LOG_DEBUG("smtrat.mis", "TRIVIAL invoked: " << x++ << std::endl);
		mis.emplace_back();
		for (const auto& it: cad.getConstraints()) mis.back().emplace(it->first);
	}
	
	template<>
	template<typename CAD>
	void MISGeneration<MISHeuristic::GREEDY>::operator()(const CAD& cad, std::vector<FormulaSetT>& mis) {
		static int x;
		SMTRAT_LOG_DEBUG("smtrat.mis", "GREEDY invoked: " << x++ << std::endl);
		mis.emplace_back();
		for (const auto& c: cad.getBounds().getOriginsOfBounds()) {
			mis.back().emplace(c);
		}
		auto cg = cad.generateConflictGraph();
		//std::cout << "rows:" << cg.numConstraints() << std::endl;
		//std::cout << "columns: " << cg.numSamples() << std::endl;
		//std::cout << "trivial columns: " << cg.numTrivialColumns() << std::endl;
		//std::cout << "unique colums: " << cg.numUniqueColumns() << std::endl;
		while (cg.hasRemainingSamples()) {
			std::size_t c = cg.getMaxDegreeConstraint();
			mis.back().emplace(cad.getConstraints()[c]->first);
			cg.selectConstraint(c);
		}
	}

	template<>
	template<typename CAD>
	void MISGeneration<MISHeuristic::GREEDY_PRE>::operator()(const CAD& cad, std::vector<FormulaSetT>& mis) {
		static int x;
		SMTRAT_LOG_DEBUG("smtrat.mis", "GREEDY_PRE invoked: " << x++ << std::endl);
		mis.emplace_back();
		for (const auto& c: cad.getBounds().getOriginsOfBounds()) {
			mis.back().emplace(c);
		}
		auto cg = cad.generateConflictGraph();
		cg = cg.removeDuplicateColumns();
		
		auto essentialConstrains = cg.selectEssentialConstraints();
		for(size_t c : essentialConstrains){
			mis.back().emplace(cad.getConstraints()[c]->first);
		}
		
		while (cg.hasRemainingSamples()) {
			std::size_t c = cg.getMaxDegreeConstraint();
			mis.back().emplace(cad.getConstraints()[c]->first);
			cg.selectConstraint(c);
		}
	}
	
	template<>
	template<typename CAD>
	void MISGeneration<MISHeuristic::HYBRID>::operator()(const CAD& cad, std::vector<FormulaSetT>& mis) {
		static int x;
		SMTRAT_LOG_DEBUG("smtrat.mis", "HYBRID invoked: " << x++ << std::endl);
		mis.emplace_back();
		for (const auto& c: cad.getBounds().getOriginsOfBounds()) {
			mis.back().emplace(c);
		}
		auto cg = cad.generateConflictGraph();
		auto essentialConstrains = cg.selectEssentialConstraints();
		for(size_t c : essentialConstrains){
			mis.back().emplace(cad.getConstraints()[c]->first);
		}
		cg = cg.removeDuplicateColumns();
		if(!cg.hasRemainingSamples()){
			return;
		}
		// Apply greedy algorithm as long as more than 6 constraints remain
		while (cg.numRemainingConstraints() > 6 && cg.hasRemainingSamples()) {
			std::size_t c = cg.getMaxDegreeConstraint();
			mis.back().emplace(cad.getConstraints()[c]->first);
			cg.selectConstraint(c);
		}

		// Find the optimum solution for the remaining constraints
		auto remaining = cg.getRemainingConstraints();
		for(size_t coverSize = 0; coverSize <= remaining.size(); coverSize++){
			std::vector<bool> selection(remaining.size() - coverSize, false);
			selection.resize(remaining.size(), true);
			do {
				carl::Bitset cover(0);
				cover.resize(cg.numSamples());
				for(size_t i = 0; i < selection.size(); i++) {
					if(selection[i]){
						cover |= remaining[i].second;
					}
				}
				if (cover.count() == cover.size()){
					for(size_t i = 0; i < selection.size(); i++) {
						if(selection[i]){
							mis.back().emplace(cad.getConstraints()[remaining[i].first]->first);
						}
					}
					return;
				}
			} while(std::next_permutation(selection.begin(), selection.end()));
		}
	}

	template<>
	template<typename CAD>
	void MISGeneration<MISHeuristic::GREEDY_WEIGHTED>::operator()(const CAD& cad, std::vector<FormulaSetT>& mis) {
		const static double constant_weight   = 1.0;
		const static double complexity_weight = 0.5;
		const static double activity_weight   = 10.0;

		static int x;
		SMTRAT_LOG_DEBUG("smtrat.mis", "GREEDY_WEIGHTED invoked: " << x++ << std::endl);
		mis.emplace_back();
		for (const auto& c: cad.getBounds().getOriginsOfBounds()) {
			mis.back().emplace(c);
		}
		auto cg = cad.generateConflictGraph();
		auto essentialConstrains = cg.selectEssentialConstraints();
		for(size_t c : essentialConstrains){
			mis.back().emplace(cad.getConstraints()[c]->first);
		}
		cg = cg.removeDuplicateColumns();

		auto constraints = cad.getConstraints();
		struct candidate {
			size_t constraint;
			FormulaT formula;
			double weight;
		};

		std::map<size_t, candidate> candidates;
		for(size_t i = 0; i < constraints.size(); i++){
			if(cad.isIdValid(i)){
				auto constraint = constraints[i];
				auto formula = FormulaT(constraint->first);
				double weight = constant_weight +
								complexity_weight * formula.complexity() +
								activity_weight / (1.0 + formula.activity());
				candidates[i] = candidate{
					formula,
					weight
				};
			}
		}
		SMTRAT_LOG_DEBUG("smtrat.mis", cg << std::endl);
		SMTRAT_LOG_DEBUG("smtrat.mis", "-------------- Included: ---------------" << std::endl);
		bool in = true;

		while (cg.hasRemainingSamples()) {
			auto selection = std::max_element(candidates.begin(), candidates.end(),
				[cg](pair<size_t, candidate> left, pair<size_t, candidate>right) {
					return cg.coveredSamples(left.first)/left.second.weight < cg.coveredSamples(right.first)/right.second.weight;
				}
			);
			SMTRAT_LOG_DEBUG("smtrat.mis", 
				"id: "            << selection->first << 
				"\t weight: "     << selection->second.weight <<
				"\t degree: "     << cg.coveredSamples(selection->first) << 
				"\t complexity: " << selection->second.formula.complexity() << 
				"\t activity: "   << selection->second.formula.activity() <<
				std::endl);
			mis.back().emplace(cad.getConstraints()[selection->first]->first);
			cg.selectConstraint(selection->first);
			candidates.erase(selection);
		}
	}

	template<>
	template<typename CAD>
	void MISGeneration<MISHeuristic::HYBRID_WEIGHTED>::operator()(const CAD& cad, std::vector<FormulaSetT>& mis) {
		const static double constant_weight   = 1.0;
		const static double complexity_weight = 0.5;
		const static double activity_weight   = 10.0;

		static int x;
		std::cout << "HYBRID_WEIGHTED invoked: " << x++ << std::endl;
		mis.emplace_back();
		for (const auto& c: cad.getBounds().getOriginsOfBounds()) {
			mis.back().emplace(c);
		}
		auto cg = cad.generateConflictGraph();
		auto essentialConstrains = cg.selectEssentialConstraints();
		for(size_t c : essentialConstrains){
			mis.back().emplace(cad.getConstraints()[c]->first);
		}
		cg = cg.removeDuplicateColumns();
		if(!cg.hasRemainingSamples()){
			return;
		}
		std::cout << "CG after preconditioning:" << std::endl;
		std::cout << cg << std::endl;

		auto constraints = cad.getConstraints();
		struct candidate {
			FormulaT formula;
			double weight;
		};

		std::map<size_t, candidate> candidates;
		for(size_t i = 0; i < constraints.size(); i++){
			if(cad.isIdValid(i)){
				auto constraint = constraints[i];
				auto formula = FormulaT(constraint->first);
				double weight = constant_weight +
								complexity_weight * formula.complexity() +
								activity_weight / (1.0 + formula.activity());
				candidates[i] = candidate{
					formula,
					weight
				};
			}
		}
		std::cout << "-------------- selecting greedily: ---------------" << std::endl;
		bool in = true;

		// Apply greedy algorithm as long as more than 6 constraints remain
		while (cg.numRemainingConstraints() > 6 && cg.hasRemainingSamples()) {
			auto selection = std::max_element(candidates.begin(), candidates.end(),
				[cg](pair<size_t, candidate> left, pair<size_t, candidate>right) {
					return cg.coveredSamples(left.first)/left.second.weight < cg.coveredSamples(right.first)/right.second.weight;
				}
			);
			SMTRAT_LOG_DEBUG("smtrat.mis", 
				"id: "            << selection->first << 
				"\t weight: "     << selection->second.weight <<
				"\t degree: "     << cg.coveredSamples(selection->first) << 
				"\t complexity: " << selection->second.formula.complexity() << 
				"\t activity: "   << selection->second.formula.activity() <<
				std::endl);
			mis.back().emplace(cad.getConstraints()[selection->first]->first);
			cg.selectConstraint(selection->first);
			candidates.erase(selection);
		}
		std::cout << "--------------------------------------------------" << std::endl;
		std::cout << "CG after greedy:" << std::endl;
		std::cout << cg << std::endl;

		// Find the optimum solution for the remaining constraints
		double bestWeight = INFINITY;
		auto remaining = cg.getRemainingConstraints();
		std::vector<bool> bestSelection(remaining.size(), true);
		for(size_t coverSize = 0; coverSize <= remaining.size(); coverSize++){
			std::vector<bool> selection(remaining.size() - coverSize, false);
			selection.resize(remaining.size(), true);
			do {
				carl::Bitset cover(0);
				cover.resize(cg.numSamples());
				for(size_t i = 0; i < selection.size(); i++) {
					if(selection[i]){
						cover |= remaining[i].second;
					}
				}
				if (cover.count() == cover.size()){
					double weight = 0.0;
					for(size_t i = 0; i < selection.size(); i++) {
						if(selection[i]){
							weight += candidates[remaining[i].first].weight;
						}
					}
					if(weight < bestWeight){
						bestWeight = weight;
						bestSelection = selection;
					}
				}
			} while(std::next_permutation(selection.begin(), selection.end()));
		}
		std::cout << "-------------- selecting optimally: ---------------" << std::endl;
		for(size_t i = 0; i < bestSelection.size(); i++) {
			if(bestSelection[i]){
				std::cout <<
					"id: "            << remaining[i].first << 
					"\t weight: "     << candidates[remaining[i].first].weight <<
					"\t degree: "     << cg.coveredSamples(remaining[i].first) << 
					"\t complexity: " << candidates[remaining[i].first].formula.complexity() << 
					"\t activity: "   << candidates[remaining[i].first].formula.activity() <<
					std::endl;
				mis.back().emplace(cad.getConstraints()[remaining[i].first]->first);
			}
		}
	}
}
}
