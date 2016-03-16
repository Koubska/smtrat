#pragma once

#include <iostream>
#include <map>
#include <vector>

#include "../Common.h"
#include "BaseProjection.h"
#include "Projection_NO.h"

namespace smtrat {
namespace cad {
	
	template<typename Settings>
	class Projection<Incrementality::SIMPLE, Backtracking::ORDERED, Settings>: public Projection<Incrementality::NONE, Backtracking::ORDERED, Settings> {
	private:
		using Super = Projection<Incrementality::NONE, Backtracking::ORDERED, Settings>;
		using QueueEntry = std::pair<UPoly,std::size_t>;
		
		struct PolynomialComparator {
			bool operator()(const QueueEntry& lhs, const QueueEntry& rhs) const {
				return rhs.first < lhs.first;
			}
		};
		
		PriorityQueue<QueueEntry, PolynomialComparator> mQueue;
	public:
		void reset(const std::vector<carl::Variable>& vars) {
			Super::reset(vars);
			mQueue.clear();
		}
		void addPolynomial(const UPoly& p, std::size_t cid) {
			mQueue.push(QueueEntry(p, cid));
		}
		void removePolynomial(const UPoly& p, std::size_t cid, const std::function<void(std::size_t,SampleLiftedWith)>& callback) {
			auto it = mQueue.find(QueueEntry(p, cid));
			if (it != mQueue.end()) {
				mQueue.erase(it);
			} else {
				Super::removePolynomial(p, cid, callback);
			}
		}
		
		bool projectNewPolynomial(std::size_t level, const ConstraintSelection& ps = Bitset(true)) {
			std::size_t oldSize = Super::size(level);
			while (!mQueue.empty()) {
				SMTRAT_LOG_DEBUG("smtrat.cad.projection", "Using next polynomial " << mQueue.top() << " from " << mQueue);
				Super::addPolynomial(mQueue.top().first, mQueue.top().second);
				mQueue.pop();
				if (Super::size(level) != oldSize) return true;
			}
			return false;
		}
		
		template<typename S>
		friend std::ostream& operator<<(std::ostream& os, const Projection<Incrementality::SIMPLE, Backtracking::ORDERED, S>& p) {
			os << "Queue: " << p.mQueue << std::endl;
			return os << Projection<Incrementality::NONE, Backtracking::ORDERED, S>(p);
		}
	};
}
}