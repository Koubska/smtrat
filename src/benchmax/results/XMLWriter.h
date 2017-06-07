#pragma once

#include <fstream>
#include <map>
#include <string>
#include "../utils/durations.h"
#include "../utils/strings.h"

namespace benchmax {

class XMLWriter {
private:
	std::ofstream mFile;

	void replace(std::string& s, const std::string& pattern, const std::string& replacement) const {
		std::size_t pos = 0;
		while ((pos = s.find(pattern, pos)) != std::string::npos) {
			s.replace(pos, pattern.length(), replacement);
			pos += replacement.length();
		}
	}
	std::string sanitize(const std::string& s, bool eliminateSlashes = false) const {
		std::string res(s);
		replace(res, "<", "&lt;");
		replace(res, ">", "&gt;");
		if (eliminateSlashes) replace(res, "/", ".");
		return res;
	}
	std::string sanitizeTool(const Tool* tool) const {
		return sanitize(removePrefix(tool->binary().native(), Settings::toolsPrefix), true);
	}
	std::string sanitizeFile(const fs::path& file, const std::string& prefix) const {
		return sanitize(removePrefix(file.native(), prefix), true);
	}
public:
	XMLWriter(const std::string& filename): mFile(filename) {
		mFile << "<?xml version=\"1.0\"?>" << std::endl;
		mFile << "<benchmarksets>" << std::endl;
	}

	template<typename Results>
	void write(const std::map<const Tool*, std::size_t>& tools, const Results& results) {
		mFile << "\t<solvers>" << std::endl;
		std::set<std::string> toolNames;
		for (const auto& tool: tools) toolNames.insert(sanitizeTool(tool.first));
		for (const auto& tool: toolNames) {
			mFile << "\t\t<solver solver_id=\"" << tool << "\" />" << std::endl;
		}
		mFile << "\t</solvers>" << std::endl;
		
		for (const auto& res: results) {
			fs::path setBaseDir = res.first;
			mFile << "\t<benchmarkset name=\"" << sanitizeFile(setBaseDir, Settings::pathPrefix) << "\">" << std::endl;
			for (const auto& file: res.second.files) {
				fs::path filename = file.first;
				mFile << "\t\t<benchmarkfile name=\"" << sanitizeFile(filename, setBaseDir.native()) << "\">" << std::endl;
				for (const auto& tool: tools) {
					std::pair<std::size_t, std::size_t> resultID(tool.second, file.second);
					auto it = res.second.data.find(resultID);
					if (it == res.second.data.end()) continue;
					mFile << "\t\t\t<run solver_id=\"" << sanitizeTool(tool.first) << "\" timeout=\"" << seconds(Settings::timeLimit).count() << "s\">" << std::endl;
					if (!it->second.additional.empty()) {
						mFile << "\t\t\t\t<runtimestats>" << std::endl;
						mFile << "\t\t\t\t\t<module name=\"All\">" << std::endl;
						for (const auto& stat: it->second.additional) {
							mFile << "\t\t\t\t\t\t<stat name=\"" << sanitize(stat.first) << "\" value=\"" << stat.second << "\" />" << std::endl;
						}
						mFile << "\t\t\t\t\t</module>" << std::endl;
						mFile << "\t\t\t\t</runtimestats>" << std::endl;
					}
					mFile << "\t\t\t\t<results>" << std::endl;
					mFile << "\t\t\t\t\t<result name=\"runtime\" type=\"msec\">" << milliseconds(it->second.time).count() << "</result>" << std::endl;
					mFile << "\t\t\t\t\t<result name=\"exitcode\" type=\"int\">" << it->second.exitCode << "</result>" << std::endl;
					mFile << "\t\t\t\t\t<result name=\"answer\" type=\"\">" << it->second.status << "</result>" << std::endl;
					mFile << "\t\t\t\t</results>" << std::endl;
					mFile << "\t\t\t</run>" << std::endl;
				}
				mFile << "\t\t</benchmarkfile>" << std::endl;
			}
			mFile << "\t</benchmarkset>" << std::endl;
		}
	}

	~XMLWriter() {
		mFile << "</benchmarksets>" << std::endl;
		mFile.close();
	}
};

}
