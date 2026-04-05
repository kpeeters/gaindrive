
#include "stamp.hh"

#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>

std::string stamp(std::string label)
	{
	std::time_t t = std::time(nullptr);
	std::tm tm = *std::localtime(&t);
	std::ostringstream ss;
	ss << std::put_time(&tm, "%F %T") << " "
		<< std::right << std::setw(15) << label << " ";
	return ss.str();
	}
