#pragma once

#include <smtrat-common/statistics/Statistics.h>

#ifdef SMTRAT_DEVOPTION_Statistics

namespace smtrat {
namespace mcsat {
namespace onecell {

class OCStatistics : public Statistics {
private:
	std::size_t mExplanationCalled = 0;
	std::size_t mExplanationSuccess = 0;

public:
	bool enabled() const {
		return true;
	}

	void collect() {
		Statistics::addKeyValuePair("explanation_called", mExplanationCalled);
		Statistics::addKeyValuePair("explanation_success", mExplanationSuccess);
	}

	void explanationCalled() {
		++mExplanationCalled;
	}

	void explanationSuccess() {
		++mExplanationSuccess;
	}
};

}
}
}

#endif
