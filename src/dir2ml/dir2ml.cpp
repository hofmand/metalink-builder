// dir2ml.cpp : https://tools.ietf.org/html/rfc5854
//

#include "stdafx.h"

// The following flag sorts the output more like hashdeep/md5deep.
//#define SORT_LIKE_HASHDEEP

// 'SORT_LIKE_WINDOWS' sorts like Windows Explorer.
#ifdef _WIN32
//#define SORT_LIKE_WINDOWS true
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>  
#include <vector>

#ifdef _WIN32
#include "shlwapi.h"
#define stat64 _wstat64
#endif

#include "windows.h"

// https://github.com/B-Con/crypto-algorithms
extern "C" {
#include "crypto-algorithms\base64.h"
#include "crypto-algorithms\md5.h"
#include "crypto-algorithms\sha1.h"
#include "crypto-algorithms\sha256.h"
}

// https://github.com/zeux/pugixml
#include "pugixml/src/pugixml.hpp"

// https://github.com/cxong/tinydir
#include "tinydir/tinydir.h"

// https://uriparser.github.io/
#include "uriparser\Uri.h"

constexpr wchar_t* APP_NAME = L"dir2ml";
constexpr wchar_t* VERSION_NO = L"0.7.4-beta.5";

//////////////////////////////////////////////////////////////////////////
//
// dir2ml: read a directory structure and create a metalinks file.
//
//////////////////////////////////////////////////////////////////////////
//
// Example Usage:
//
// dir2ml -d ./MyMirror -u ftp://ftp.example.com -c us -o MyMirror.meta4
//
//////////////////////////////////////////////////////////////////////////
//
// Example output file:
//
// <?xml version="1.0" encoding="UTF-8"?>
// <metalink xmlns="urn:ietf:params:xml:ns:metalink" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:noNamespaceSchemaLocation="metalink4.xsd">
//   <file name="example.ext">
//     <size>14471447</size>
//     <generator>dir2ml/0.1.0</generator>
//     <updated>2010-05-01T12:15:02Z</updated>
//     <hash type="sha-256">17bfc4a6058d2d7d82db859c8b0528c6ab48d832fed620ed49fb3385dbf1684d</hash>
//     <url location="us">ftp://ftp.example.com/example.ext</url>
//	 </file>
//   <file name="subdir/example2.ext">
//     <size>14471447</size>
//     <generator>dir2ml/0.1.0</generator>
//     <updated>2010-05-01T12:15:02Z</updated>
//     <hash type="sha-256">f44bcce2a9c2aa3f73ddc853ad98f87cd8e7cee5b5c18719ebb220da3fd4dbc9</hash>
//     <url location="us">ftp://ftp.example.com/subdir/example2.ext</url>
//	 </file>
// </metalink>
//
//////////////////////////////////////////////////////////////////////////

using namespace std;

#if __cplusplus < 201703L && defined _MSC_VER
namespace fs = std::experimental::filesystem::v1;
#else
namespace fs = std::filesystem;
#endif

typedef unsigned int sha256_data_size;
typedef unsigned long md5_data_size;
typedef sha256_data_size max_buf_size; // because sizeof(unsigned int) <= sizeof(unsigned long)

typedef uint_fast16_t process_dir_flags_t;

constexpr process_dir_flags_t FLAG_VERBOSE                = 0x0001;
constexpr process_dir_flags_t FLAG_MD5                    = 0x0002;
constexpr process_dir_flags_t FLAG_SHA1                   = 0x0004;
constexpr process_dir_flags_t FLAG_SHA256                 = 0x0008;
constexpr process_dir_flags_t FLAG_NI_URL                 = 0x0010;
constexpr process_dir_flags_t FLAG_CONSOLIDATE            = 0x0020;
constexpr process_dir_flags_t FLAG_FIND_DUPES             = 0x0040;
constexpr process_dir_flags_t FLAG_FILE_URL               = 0x0080;
constexpr process_dir_flags_t FLAG_BASE_URL               = 0x0100;
constexpr process_dir_flags_t FLAG_IGNORE_DATES_AND_TIMES = 0x0200;
constexpr process_dir_flags_t FLAG_IGNORE_TIMES           = 0x0400; // but respect dates

constexpr process_dir_flags_t FLAG_ALL_HASHES = FLAG_MD5 | FLAG_SHA1 | FLAG_SHA256;
constexpr process_dir_flags_t FLAG_ALL_URL_TYPES = FLAG_NI_URL | FLAG_FILE_URL | FLAG_BASE_URL;
constexpr process_dir_flags_t FLAG_FIND_OR_CONSOLIDATE_DUPES = FLAG_CONSOLIDATE | FLAG_FIND_DUPES;
constexpr process_dir_flags_t FLAG_IGNORE_DATES_OR_TIMES = FLAG_IGNORE_DATES_AND_TIMES | FLAG_IGNORE_TIMES;

constexpr process_dir_flags_t FLAG_DEFAULT = FLAG_SHA256;

constexpr size_t PROGRESS_MARKER_BYTES = 1000000; // 1MB (not MiB)

struct fileNodeInfo
{
	fileNodeInfo()
		: fileSize(0)
		, urlMap(make_shared<url_to_scheme_map_t>())
	{
	}

	wstring fileName;
	fs::path filePath;
	uint_fast64_t fileSize;
	__time64_t fileMTime;
	wstring md5HashStr;
	wstring sha1HashStr;
	wstring sha256HashStr;
	typedef map<wstring,wstring> url_to_scheme_map_t;
	shared_ptr<url_to_scheme_map_t> urlMap;
	wstring niStr;
};

struct ProcessDirContext
{
	ProcessDirContext(fs::path const& rootDir)
		: rootDir_(rootDir)
		, numFiles(0)
		, numBytes(0)
		, numCollisions(0)
		, dirDepth(0)
	{
		_ASSERTE(rootDir.wstring().back() == fs::path::preferred_separator);
	}

	const fs::path rootDir_;
	size_t numFiles;
	uint_fast64_t numBytes;
	size_t numCollisions;
	size_t dirDepth;

	typedef list<fileNodeInfo> file_node_info_list_t;
	file_node_info_list_t fileNodeInfoList;

	typedef multimap<wstring, file_node_info_list_t::iterator> hash_to_filenode_map_t;
	hash_to_filenode_map_t hashToFileNodeMap;
};

struct case_insensitive_compare
{
	bool operator()(std::wstring const& lhsIn, std::wstring const& rhsIn) const
	{
#ifdef SORT_LIKE_WINDOWS
		return StrCmpLogicalW(lhsIn.c_str(), rhsIn.c_str()) == -1;
#else // !define SORT_LIKE_WINDOWs
		wchar_t const* lhs = lhsIn.c_str();
		wchar_t const* rhs = rhsIn.c_str();
		for (; *lhs != L'\0' && *rhs != L'\0'; ++lhs, ++rhs)
		{
			if (tolower(*lhs) != tolower(*rhs))
			{
#ifdef SORT_LIKE_HASHDEEP
				// Alpha characters before '_'
				// Not sure how else the sorting is different..
				if (*lhs == L'_' && *rhs != L'_')
					return false;
				else if (*lhs != L'_' && *rhs == L'_')
					return true;
				else
#endif // SORT_LIKE_HASHDEEP
				return (tolower(*lhs) < tolower(*rhs));
			}
			else if (*lhs != *rhs)
			{
				if (*(lhs + 1) == L'\0' && *(rhs + 1) == L'\0')
				{
					return (*lhs < *rhs);
				}
			}
		}
		return (tolower(*lhs) < tolower(*rhs));
#endif // !defined SORT_LIKE_WINDOWS
	}
};

enum PATHTYPE { PATH_IS_FILE, PATH_IS_DIR };
typedef map<wstring, PATHTYPE, case_insensitive_compare> PATH_NAME_TYPE_MAP; // sort alphabetically...

template<typename InputIterator1, typename InputIterator2>
bool
range_equal(InputIterator1 first1, InputIterator1 last1,
	InputIterator2 first2, InputIterator2 last2)
{
	while (first1 != last1 && first2 != last2)
	{
		if (*first1 != *first2) return false;
		++first1;
		++first2;
	}
	return (first1 == last1) && (first2 == last2);
}

// files_identical() returns true if they contain the same content
bool files_identical(const wstring& filename1, const wstring& filename2)
{
	ifstream file1(filename1);
	ifstream file2(filename2);

	istreambuf_iterator<char> begin1(file1);
	istreambuf_iterator<char> begin2(file2);

	istreambuf_iterator<char> end;

	return range_equal(begin1, end, begin2, end);
}

// https://tools.ietf.org/html/rfc3986
wstring url_encode(const wstring &fileName, const wstring& scheme)
{
	wstring encodedUrl;
	if (scheme == L"ni")
		encodedUrl = wstring(L"ni:///") + fileName;
	else
	{
		const size_t charsNeeded = 8 + 3 * fileName.size() + 1;
		wchar_t* pBuffer = new wchar_t[charsNeeded];

		int retVal;
		if (scheme.empty())
#ifdef _WIN32
			retVal = uriWindowsFilenameToUriStringW(fileName.c_str(), pBuffer);
#else // !_WIN32
			retVal = uriUnixFilenameToUriStringW(fileName.c_str(), pBuffer);
#endif // !_WIN32
		else
		{
			size_t i = fileName.find(L"://");
			retVal = uriUnixFilenameToUriStringW(fileName.c_str() + i + 3, pBuffer);
		}

		if (retVal != URI_SUCCESS)
		{
			wcerr << L"Failed to convert filename \"" << fileName << L"\" to URI!" << endl;
			return fileName;
		}

		if (scheme.empty())
			encodedUrl = pBuffer;
		else
			encodedUrl = scheme + L"://" + pBuffer;
		delete[] pBuffer;
	}

	return encodedUrl;
}

enum BYTES_UNIT_TYPE
{
	METRIC_UNITS, // e.g. "kB"
	IEC_UNITS     // e.g. "kiB"
};

uint_fast64_t minBytesToNeedHumanReadableString(BYTES_UNIT_TYPE unitType)
{
	_ASSERTE(unitType == METRIC_UNITS || unitType == IEC_UNITS);

	if (unitType == IEC_UNITS)
		return 1024;
	else
		return 1000;
}

wstring bytesToHumanReadableString(uint_fast64_t numBytes, BYTES_UNIT_TYPE unitType)
{
	wostringstream buf;

	uint_fast64_t multiplier = minBytesToNeedHumanReadableString(unitType);
	if (numBytes >= multiplier * multiplier * multiplier * multiplier * multiplier)
	{
		buf << fixed << setprecision(2)
			<< numBytes / static_cast<double>(multiplier * multiplier * multiplier * multiplier * multiplier)
			<< L" P";
		if (unitType == IEC_UNITS)
			buf << L"i";
		buf << L"B";
	}if (numBytes >= multiplier * multiplier * multiplier * multiplier)
	{
		buf << fixed << setprecision(2)
			<< numBytes / static_cast<double>(multiplier * multiplier * multiplier * multiplier)
			<< L" T";
		if (unitType == IEC_UNITS)
			buf << L"i";
		buf << L"B";
	}
	else if (numBytes >= multiplier * multiplier * multiplier)
	{
		buf << fixed << setprecision(2)
			<< numBytes / static_cast<double>(multiplier * multiplier * multiplier)
			<< L" G";
		if (unitType == IEC_UNITS)
			buf << L"i";
		buf << L"B";
	}
	else if (numBytes >= multiplier * multiplier)
	{
		buf << fixed << setprecision(2)
			<< numBytes / static_cast<double>(multiplier * multiplier)
			<< L" M";
		if (unitType == IEC_UNITS)
			buf << L"i";
		buf << L"B";
	}
	else if (numBytes >= multiplier)
	{
		buf << fixed << setprecision(2)
			<< numBytes / static_cast<double>(multiplier)
			<< L" k";
		if (unitType == IEC_UNITS)
			buf << L"i";
		buf << L"B";
	}
	else
	{
		buf << numBytes << L" B";
	}

	return buf.str();
}

typedef uint_fast16_t check_metadata_flag_t;
constexpr check_metadata_flag_t CHECK_DATES = 0x1;
constexpr check_metadata_flag_t CHECK_TIMES = 0x2;

static bool FileMetadataMatches(fileNodeInfo const& file1, fileNodeInfo const& file2, check_metadata_flag_t flags)
{
	// CHECK_TIMES cannot function without CHECK_DATES
	_ASSERTE((flags & (CHECK_DATES | CHECK_TIMES)) != CHECK_TIMES);

	// File sizes must always match
	if (file1.fileSize != file2.fileSize)
		return false;

	// If we're ignoring file dates, the files match
	if (!(flags & CHECK_DATES))
		return true;

	// If we're checking file dates and times, it's a simple check
	if (flags & CHECK_TIMES)
		return file1.fileMTime == file2.fileMTime;

	// We're checking file dates only and ignoring times.

	tm timeStruct1;
	_localtime64_s(&timeStruct1, &file1.fileMTime);

	tm timeStruct2;
	_localtime64_s(&timeStruct2, &file2.fileMTime);

	return timeStruct1.tm_year == timeStruct2.tm_year
		&& timeStruct1.tm_yday == timeStruct2.tm_yday;
}

// 'relativePath' is the directory path relative to ctx.rootDir_
void ProcessDir( fs::path const& relativePath,
                 pugi::xml_node& xmlRootNode,
                 wstring const& country,
                 wstring const& baseURL,
                 wstring const& baseUrlType,
                 wstring const& fileUrlBase,
                 ProcessDirContext& ctx,
                 process_dir_flags_t flags )
{
	bool wantVerbose = ((flags & FLAG_VERBOSE) != 0);

	fs::path inputDirName = ctx.rootDir_;
	inputDirName.append(relativePath);
	if (inputDirName.wstring().back() != fs::path::preferred_separator)
		inputDirName += fs::path::preferred_separator;

	++ctx.dirDepth;

	PATH_NAME_TYPE_MAP pathContents;
	{
		tinydir_dir inputDir;
		if (0 != tinydir_open(&inputDir, inputDirName.c_str()))
		{
			wcerr << L"Couldn't open input directory \"" << inputDirName
				<< L"\"!" << endl;
			return;
		}

		while(inputDir.has_next)
		{
			tinydir_file file;
			tinydir_readfile(&inputDir, &file);

			wstring fileName = file.name;
			if (fileName != L"." && fileName != L"..")
				pathContents[file.name] = file.is_dir ? PATH_IS_DIR : PATH_IS_FILE;

			tinydir_next(&inputDir);
		}

		tinydir_close(&inputDir);
	}

	if (wantVerbose)
		wcout << setw(ctx.dirDepth - 1) << setfill(L'|') << L"+" << inputDirName << endl;

	// Reuse the same file buffer to avoid reallocating memory.
	static vector<char> fileBuffer;
	constexpr max_buf_size bufferSize = 8192;

	uint_fast64_t oldNumBytes = 0;

	for(auto it = pathContents.begin(); it != pathContents.end(); ++it)
	{
		wstring fileName = it->first;
		fs::path filePathRel = relativePath;
		if (!filePathRel.empty() && filePathRel.wstring().back() != fs::path::preferred_separator)
			filePathRel += fs::path::preferred_separator;
		filePathRel += fileName;

		if (it->second == PATH_IS_DIR)
		{
			ProcessDir(filePathRel, xmlRootNode,
				country, baseURL, baseUrlType, fileUrlBase, ctx, flags);
		}
		else
		{
			++ctx.numFiles;

			// <file name="example.ext">
			fs::path filePathAbs(inputDirName);
			filePathAbs.append(fileName);

			if (wantVerbose)
				wcout << setw(ctx.dirDepth-1) << setfill(L'|') << L" " << fileName;

			fileNodeInfo thisNode;

			thisNode.fileName = fs::path(filePathRel).generic_wstring();
			thisNode.filePath = filePathAbs;

			// <size>14471447</size>
			ifstream inFile(filePathAbs, ifstream::ate | ifstream::binary);

#ifdef _WIN32
			// Use _wstat64 to make 32-bit windows build report the
			// correct file size, because ifstream::tellg() is limited
			// to 32 bits in 32-bit builds.
			{
				struct __stat64 fileInfo;
				switch (stat64(filePathAbs.c_str(), &fileInfo))
				{
				case 0:
					thisNode.fileSize = fileInfo.st_size;
					thisNode.fileMTime = fileInfo.st_mtime;
					break;
				case ENOENT:
					wcout << L"File \"" << filePathAbs << "\" not found!" << endl;
					return;
				default:
					wcout << L"Undefined error reading file \"" << filePathAbs << L"\"!" << endl;
					return;
				}
			}
#else
			// Not sure how to do the above in other operating systems
			thisNode.fileSize = inFile.tellg();
			thisNode.fileMTime = ?;
#endif

			ctx.numBytes += thisNode.fileSize;

			// <url location="us">ftp://ftp.example.com/example.ext</url>
			if (flags & FLAG_BASE_URL)
			{
				fs::path filePathAbs(baseURL);
				filePathAbs.append(filePathRel);

				wstring::size_type i = baseURL.find(L"://");
				wstring schemeName = baseURL.substr(0, i);

				thisNode.urlMap.get()->insert(make_pair(filePathAbs.generic_wstring(), schemeName));
			} // end <url>..</url>

			// <url location="us">ftp://ftp.example.com/example.ext</url>
			if (flags & FLAG_FILE_URL)
			{
				fs::path fileUrlAbs = fileUrlBase;
				fileUrlAbs += fs::path::preferred_separator;
				fileUrlAbs += filePathRel;

				thisNode.urlMap.get()->insert(
					make_pair(fileUrlAbs.wstring(),wstring()));
			} // end <url>..</url>

			// <hash type="md5">05c7d97c0e3a16ced35c2d9e4554f906</hash>
			// <hash type="sha-1">a97fcf6ba9358f8a6f62beee4421863d3e52b080</hash>
			// <hash type="sha-256">f0ad929cd259957e160ea442eb80986b5f01...</hash>
			if (flags & (FLAG_ALL_HASHES|FLAG_FIND_OR_CONSOLIDATE_DUPES))
			{
				MD5_CTX ctxMD5;
				md5_init(&ctxMD5);

				SHA1_CTX ctxSHA_1;
				sha1_init(&ctxSHA_1);

				SHA256_CTX ctxSHA_256;
				sha256_init(&ctxSHA_256);

				inFile.seekg(0, ios::beg);
				{
					while (!inFile.eof())
					{
						fileBuffer.resize(bufferSize);
						inFile.read(&fileBuffer[0], bufferSize);
						max_buf_size bytesRead = static_cast<max_buf_size>(inFile.gcount());
						if (bytesRead > 0)
						{
							if (flags & FLAG_MD5)
							{
								md5_update(&ctxMD5, reinterpret_cast<const uint8_t*>(&fileBuffer[0]),
									bytesRead);
							}

							if (flags & FLAG_SHA1)
							{
								sha1_update(&ctxSHA_1,
									reinterpret_cast<const uint8_t*>(&fileBuffer[0]),
									bytesRead);
							}

							if ((flags & FLAG_SHA256 || flags & FLAG_FIND_OR_CONSOLIDATE_DUPES))
							{
								sha256_update(&ctxSHA_256,
									reinterpret_cast<const uint8_t*>(&fileBuffer[0]),
									bytesRead);
							}

							// Every 1MB let's output a '.' just to prove we're still alive
							auto numTicks = ((oldNumBytes + bytesRead) / PROGRESS_MARKER_BYTES) - (oldNumBytes / PROGRESS_MARKER_BYTES);
							if(numTicks != 0)
								wcout << wstring(static_cast<size_t>(numTicks), L'.');
						} // end if (bytesRead > 0)

						oldNumBytes += bytesRead;
					} // end while (!inFile.eof())
				} // end scope

				// MD5
				static vector<uint8_t> md5Digest(MD5_BLOCK_SIZE);
				fill(md5Digest.begin(), md5Digest.end(), 0);
				if (flags & FLAG_MD5)
				{
					wostringstream buf;
					{
						md5_final(&ctxMD5, &md5Digest[0]);
						for (size_t i = 0; i < MD5_BLOCK_SIZE; ++i)
							buf << hex << setw(2) << setfill(L'0') << md5Digest[i];
					}

					thisNode.md5HashStr = buf.str();
				} // end MD5

				// SHA-1
				static vector<uint8_t> sha1Digest(SHA1_BLOCK_SIZE);
				fill(sha1Digest.begin(), sha1Digest.end(), 0);
				if (flags & FLAG_SHA1)
				{
					wostringstream buf;
					{
						sha1_final(&ctxSHA_1, &sha1Digest[0]);
						for (size_t i = 0; i < SHA1_BLOCK_SIZE; ++i)
							buf << hex << setw(2) << setfill(L'0') << sha1Digest[i];
					}

					thisNode.sha1HashStr = buf.str();
				} // end SHA-1

				// SHA-256
				static vector<uint8_t> sha256Digest(SHA256_BLOCK_SIZE);
				fill(sha256Digest.begin(), sha256Digest.end(), 0);
				if (flags & (FLAG_SHA256 | FLAG_FIND_OR_CONSOLIDATE_DUPES))
				{
					wostringstream buf;
					{
						sha256_final(&ctxSHA_256, &sha256Digest[0]);
						for (size_t i = 0; i < SHA256_BLOCK_SIZE; ++i)
							buf << hex << setw(2) << setfill(L'0') << sha256Digest[i];
					}

					thisNode.sha256HashStr = buf.str();
				}

				// https://tools.ietf.org/html/rfc6920
				// Example URL: ni:///sha-256;UyaQV-Ev4rdLoHyJJWCi11OHfrYv9E1aGQAlMO2X_-Q
				if ((flags & FLAG_NI_URL) && (flags & FLAG_SHA256))
				{
					// Construct the URL in MBCS
					auto bufSize = base64_encode(&sha256Digest[0], nullptr, sha256Digest.size(), 0);
					string url8(bufSize, ' ');
					base64_encode(&sha256Digest[0],
						reinterpret_cast<uint8_t*>(&url8[0]), sha256Digest.size(), 0);
					url8.erase(url8.find_last_not_of("=")+1); // trim trailing "=" padding characters
					url8.insert(0, "sha-256;");

					// Convert to Unicode for pugixml
					wstring_convert<codecvt_utf8<wchar_t> > conv;
					wstring url16 = conv.from_bytes(url8);
					thisNode.urlMap.get()->insert(make_pair(url16,L"ni"));
				}

				if (flags & FLAG_FIND_OR_CONSOLIDATE_DUPES)
				{
					bool foundDupes(false);
					auto itHashToFileNode = ctx.hashToFileNodeMap.find(thisNode.sha256HashStr);
					while (itHashToFileNode != ctx.hashToFileNodeMap.end()
						&& itHashToFileNode->first == thisNode.sha256HashStr)
					{
						check_metadata_flag_t metadataFlags(0);
						if (flags & FLAG_IGNORE_TIMES) // ignore times but respect dates?
							metadataFlags |= CHECK_DATES;
						else if (!(flags & FLAG_IGNORE_DATES_AND_TIMES)) // respect both dates and times?
							metadataFlags |= (CHECK_DATES | CHECK_TIMES);

						if(FileMetadataMatches(*itHashToFileNode->second, thisNode, metadataFlags))
						{
							for (auto it = (*thisNode.urlMap.get()).begin();
								it != (*thisNode.urlMap.get()).end(); ++it)
							{
								if (itHashToFileNode->second->urlMap.get()->find(it->first)
									!= itHashToFileNode->second->urlMap.get()->end())
								{
									foundDupes = true;
									break;
								}
							}
							if (!foundDupes)
							{
								for (auto it = itHashToFileNode->second->urlMap.get()->begin();
									it != itHashToFileNode->second->urlMap.get()->end(); ++it)
								{
									if (thisNode.urlMap.get()->find(it->first)
										!= thisNode.urlMap.get()->end())
									{
										foundDupes = true;
										break;
									}
								}
							}

							if (!foundDupes)
							{
								wcout << L"\n*** Possible duplicate: \"" << filePathAbs.wstring()
									<< L"\" and \"" << itHashToFileNode->second->filePath
									<< L"\" have the same size";
								if (!(flags & FLAG_IGNORE_DATES_OR_TIMES))
									wcout << L", last modified date,";
								wcout << L" and SHA-256 hash! Verifying byte by byte... ";
							}

							if (foundDupes || files_identical(itHashToFileNode->second->filePath,
								thisNode.filePath))
							{
								if (!foundDupes)
								{
									foundDupes = true;
									wcout << L"confirmed." << endl;
								}

								// Add this node's URL(s) to the existing XML node
								itHashToFileNode->second->urlMap.get()->insert(
									thisNode.urlMap.get()->begin(),
									thisNode.urlMap.get()->end());

								if (flags & FLAG_FIND_DUPES)
								{
									// Add the existing XML node's URL(s) to this one
									thisNode.urlMap = itHashToFileNode->second->urlMap;
								}
							}
							else
							{
								++ctx.numCollisions;
								wcout << L"busted! Congratulations, you found a SHA-256 collision!" << endl;
							}
						} // end hash and file size are identical

						++itHashToFileNode;
					}

					if (foundDupes && (flags & FLAG_CONSOLIDATE))
					{
						// Don't add this XML node
						continue;
					}
					else
					{
						// Create a new entry in the file node info list
						auto itList = ctx.fileNodeInfoList.insert(
							ctx.fileNodeInfoList.end(), thisNode);

						// Create a new entry in the hash-to-filename map
						ctx.hashToFileNodeMap.insert(itHashToFileNode,
							ProcessDirContext::hash_to_filenode_map_t::value_type
							(thisNode.sha256HashStr, itList));
					} // end if duplicates found
				} // end consolidate-duplicates
			} // end if any hashes enabled

			if (!(flags & FLAG_FIND_OR_CONSOLIDATE_DUPES))
			{
				// Create a new entry in the file node info list
				ctx.fileNodeInfoList.insert(
					ctx.fileNodeInfoList.end(), thisNode);
			}

			if(wantVerbose)
				wcout << endl;
		} // end file
	} // end files in this directory

	--ctx.dirDepth;
}

int wmain( int argc, wchar_t **argv )
{
	fs::path inputDirName;
	wstring baseURL, fileUrlBase, country, outFileName;

	bool invalidArgs(false);
	bool wantHelp(false), wantStatistics(false), wantVerbose(false);

	wcout << APP_NAME << L" " << VERSION_NO << L"\n" << endl;

	process_dir_flags_t flags(FLAG_DEFAULT);

	// Process arguments (stage 1)
	for(int a = 1; a < argc; ++a)
	{
		bool validArg(false);

		wstring argText = argv[a];
		if(argText == L"-d" || argText == L"--directory")
		{
			if (a < argc - 1)
			{
				++a;
				inputDirName = argv[a];
				validArg = true;
			}
		}
		else if (argText == L"-u" || argText == L"--base-url")
		{
			if (a < argc - 1)
			{
				++a;
				baseURL = argv[a];
				flags |= FLAG_BASE_URL;
				validArg = true;
			}
		}
		else if (argText == L"-o" || argText == L"--output")
		{
			if (a < argc - 1)
			{
				++a;
				outFileName = argv[a];
				validArg = true;
			}
		}
		else if (argText == L"-c" || argText == L"--country")
		{
			if (a < argc - 1)
			{
				++a;
				country = argv[a];
				validArg = true;
			}
		}
		else if (argText == L"-h" || argText == L"--help")
		{
			wantHelp = true;
			validArg = true;
		}
		else if (argText == L"-s" || argText == L"--show-statistics")
		{
			wantStatistics = true;
			validArg = true;
		}
		else if (argText == L"-v" || argText == L"--verbose")
		{
			wantVerbose = true;
			flags |= FLAG_VERBOSE;
			validArg = true;
		}
		else if (argText == L"--version")
		{
			validArg = true;
			return EXIT_SUCCESS;
		}
		else if (argText == L"--hash-type")
		{
			flags &= ~FLAG_ALL_HASHES;

			if (a < argc - 1)
			{
				++a;
				wstringstream ss(argv[a]);
				while (ss.good())
				{
					wstring substr;
					getline(ss, substr, L',');
					if (substr == L"md5")
					{
						flags |= FLAG_MD5;
						validArg = true;
					}
					else if (substr == L"sha1")
					{
						flags |= FLAG_SHA1;
						validArg = true;
					}
					else if (substr == L"sha256")
					{
						flags |= FLAG_SHA256;
						validArg = true;
					}
					else if (substr == L"all")
					{
						flags |= FLAG_ALL_HASHES;
						validArg = true;
					}
					else
					{
						validArg = false;
						break;
					}
				}
			}
		}
		else if (argText == L"--ni-url")
		{
			flags |= FLAG_NI_URL;
			validArg = true;
		}
		else if (argText == L"-f" || argText == L"--file-url")
		{
			flags |= FLAG_FILE_URL;
			validArg = true;
		}
		else if (argText == L"--find-duplicates")
		{
			flags |= FLAG_FIND_DUPES;
			validArg = true;
		}
		else if (argText == L"--consolidate-duplicates")
		{
			flags |= FLAG_CONSOLIDATE;
			validArg = true;
		}
		else if (argText == L"--ignore-file-dates")
		{
			flags |= FLAG_IGNORE_DATES_AND_TIMES;
			validArg = true;
		}
		else if (argText == L"--ignore-file-times")
		{
			flags |= FLAG_IGNORE_TIMES;
			validArg = true;
		}

		if (!validArg)
		{
			wcerr << L"Invalid argument: \"" << argv[a] << L"\""
				<< endl;
			invalidArgs = true;
		}
	}

	// Process arguments (stage 2)
	wstring baseUrlType;
	if (!wantHelp && !invalidArgs)
	{
		// Input directory
		if (inputDirName.empty())
		{
			wcerr << L"Missing input directory!" << endl;
			invalidArgs = true;
		}

		// At least one URL type must be supplied
		if ((flags & FLAG_ALL_URL_TYPES) == 0)
		{
			wcerr << L"Missing -u/--base-url, -f/--file-url, or --ni-url!" << endl;
			invalidArgs = true;
		}

		// Base URL
		if (flags & FLAG_BASE_URL)
		{
			auto i = baseURL.find(L"://"); // e.g. ftp://www.example.com
			if (i != wstring::npos)
			{
				baseUrlType = baseURL.substr(0, i);
			}
			else
			{
				wcerr << L"Invalid -u/--base-url!" << endl;
				invalidArgs = true;
			}
		} // end scope

		if (flags & FLAG_FILE_URL)
		{
			// Append missing trailing "/" so fs::canonical() doesn't think it's a file
			if (inputDirName.wstring().back() != fs::path::preferred_separator)
				inputDirName += fs::path::preferred_separator;

			// Get the full path of base-url
			error_code ec;
			fileUrlBase = fs::canonical(inputDirName, ec);
			if (ec)
			{
				wcerr << L"Can't get canonical path from \"" << inputDirName
					<< L"\"! ";
				cerr << ec.message() << endl;
				return EXIT_FAILURE;
			}
		} // end FLAG_FILE

		// Country code
		if (!country.empty() && country.size() != 2)
		{
			wcerr << "Invalid country code \"" << country << "\"!" << endl;
			invalidArgs = true;
		}

		// Output filename
		if (outFileName.empty())
		{
			wcerr << L"Missing output filename!" << endl;
			invalidArgs = true;
		}

		// ni
		if( ((flags & FLAG_NI_URL) != 0) && ((flags & FLAG_SHA256) == 0) )
		{
			wcerr << L"--ni-url requires --hash-type sha256!" << endl;
			invalidArgs = true;
		}

		// find/consolidate dupes
		if ((flags & FLAG_FIND_OR_CONSOLIDATE_DUPES) == FLAG_FIND_OR_CONSOLIDATE_DUPES)
		{
			wcerr << L"--find-duplicates and --consolidate-duplicates are mutually exclusive!" << endl;
			invalidArgs = true;
		}

		if ((flags & FLAG_IGNORE_DATES_OR_TIMES) && !(flags & FLAG_FIND_OR_CONSOLIDATE_DUPES))
		{
			wcerr << L"--ignore-file-dates and --ignore-file-times require --find-duplicates or --consolidate-duplicates!" << endl;
			invalidArgs = true;
		}

		if ((flags & FLAG_IGNORE_DATES_OR_TIMES) == FLAG_IGNORE_DATES_OR_TIMES)
		{
			wcerr << L"--ignore-file-dates and --ignore-file-times are mutually exclusive!" << endl;
			invalidArgs = true;
		}
	}

	if (invalidArgs && !wantHelp)
		wcout << L"\nTry:\t" << APP_NAME << " --help" << endl;

	if (wantHelp)
	{
		wcout << L"Usage:\n"
			<< L"\n"
			<< L" " << APP_NAME << " --help\n"
			<< L" " << APP_NAME << " --directory path [--base-url url] --output outfile [--country code] [--verbose]\n"
			<< L" " << APP_NAME << " -d directory-path -u base-url -o outfile [-c country-code] [-v]\n"
			<< L"\n"
			<< L"Example usage:\n"
			<< L"\n"
			<< L" " << APP_NAME << " -d ./MyMirror -u ftp://ftp.example.com -c us --sha256 -o MyMirror.meta4\n"
			<< L"\n"
			<< L"Required Arguments:\n"
			<< L"\n"
			<< L" -d, --directory directory - The directory path to process\n"
			<< L" -o, --output outfile - Output filename(.meta4 or .metalink)\n"
			<< L"\n"
			<< L"At least one of -u/--base-url, -f/--file-url, or --ni-url must be supplied.\n"
			<< L"\n"
			<< L"Optional Arguments:\n"
			<< L"\n"
			<< L" -c, --country country-code - ISO3166-1 alpha-2 two letter country code of the server specified by base-url above\n"
			<< L" -h, --help - Show this screen\n"
			<< L" -s, --show-statistics - Show statistics at the end of processing\n"
			<< L" -u, --base-url base-url - The base/root URL of an online directory containing the files. For example, ftp://ftp.example.com. " << APP_NAME << " will append the relative path of each file to the base-url.\n"
			<< L" -f, --file - Add a local source for the file, using the directory specified by `--directory` prepended by `file://`. This is useful for fingerprinting a directory or hard drive."
			<< L"   Note: on Windows, backslashes (\\) in the base-url will be replaced by forward slashes (/).\n"
			<< L" -v, --verbose - Verbose output to stdout\n"
			<< L" --hash-type hash-list - Calculate and output hash-list (comma-separated). Available hashes are md5, sha1, sha256, and all. If none are specified, sha256 is used.\n"
			<< L" --find-duplicates - Add URLs from all duplicate files to each matching metalink `file` node.\n"
			<< L" --consolidate-duplicates - Add duplicate URLs from all duplicate files to the first matching metalink `file` node and remove the other matching `file` nodes.\n"
			<< L" --ignore-file-dates - Ignore file \"last modified\" dates and times when finding or consolidating duplicates." << endl
			<< L" --ignore-file-times - Ignore file \"last modified\" times (but respect dates) when finding or consolidating duplicates." << endl
			<< L" --ni-url - Output Named Information (RFC6920) links (experimental). Requires --hash-type sha256" << endl;
	}

	if (invalidArgs)
		return EXIT_FAILURE;
	else if (wantHelp)
		return EXIT_SUCCESS;

	// <?xml version="1.0" encoding="UTF-8"?>
	pugi::xml_document xmlDoc;
	pugi::xml_node xmlDeclNode = xmlDoc.append_child(pugi::node_declaration);
	xmlDeclNode.set_name(L"xml");
	xmlDeclNode.append_attribute(L"version")
		.set_value(L"1.0");

	// <metalink xmlns="urn:ietf:params:xml:ns:metalink">
	pugi::xml_node xmlRootNode = xmlDoc.append_child(L"metalink");
	xmlRootNode.append_attribute(L"xmlns")
		.set_value(L"urn:ietf:params:xml:ns:metalink");
	xmlRootNode.append_attribute(L"xmlns:xsi")
		.set_value(L"http://www.w3.org/2001/XMLSchema-instance");
	xmlRootNode.append_attribute(L"xsi:noNamespaceSchemaLocation")
		.set_value(L"metalink4.xsd");

	// <generator>dir2ml/0.1.0</generator>
	{
		xmlRootNode.append_child(L"generator")
			.append_child(pugi::node_pcdata)
			.set_value((wstring(APP_NAME) + L"/" + VERSION_NO).c_str());
	}

	// <published>2010-05-01T12:15:02Z</published>
	{
		wstring currentDate;
		{
			time_t now;
			time(&now);
			wchar_t buf[sizeof L"2011-10-08T07:07:09Z"];
			struct tm newTime;
			gmtime_s(&newTime, &now);
			wcsftime(buf, sizeof buf, L"%FT%TZ", &newTime);
			currentDate = buf;
		}

		xmlRootNode.append_child(L"published")
			.append_child(pugi::node_pcdata)
			.set_value(currentDate.c_str());
	}

	auto startTick = GetTickCount64();

	ProcessDirContext ctx(inputDirName);
	ProcessDir(L"", xmlRootNode, country, baseURL,
		baseUrlType, fileUrlBase, ctx, flags);

	auto endTick = GetTickCount64();

	wstring statistics;
	{
		wostringstream buf;

		double numSeconds = (endTick - startTick) / 1000.;
		double Mbps = ((ctx.numBytes / 1e6) * 8) / numSeconds;
		wstring resultsTitle = wstring(APP_NAME) + L" v" + VERSION_NO + L" results:";
		buf << L"\n" << resultsTitle
			<< L"\n" << setw(resultsTitle.length()) << setfill(L'=') << L"=";

		buf << L"\n# of input files: ";
		buf.imbue(std::locale(""));
		buf << ctx.numFiles;

		buf << L"\n# of input bytes: " << ctx.numBytes;
		if (ctx.numBytes >= minBytesToNeedHumanReadableString(METRIC_UNITS)
			|| ctx.numBytes >= minBytesToNeedHumanReadableString(IEC_UNITS))
		{
			buf << L" (";
			bool needSeparator(false);
			if (ctx.numBytes >= minBytesToNeedHumanReadableString(METRIC_UNITS))
			{
				buf << bytesToHumanReadableString(ctx.numBytes, METRIC_UNITS);
				needSeparator = true;
			}
			if (ctx.numBytes >= minBytesToNeedHumanReadableString(IEC_UNITS))
			{
				if (needSeparator)
					buf << L"/";
				buf << bytesToHumanReadableString(ctx.numBytes, IEC_UNITS);
			}
			buf << L")";
		}

		buf << L"\n# of seconds: " << numSeconds;
		if (numSeconds > 60)
		{
			buf << L" (";
			size_t numHours = static_cast<size_t>(numSeconds) / (60 * 60);
			size_t numMinutes = static_cast<size_t>(numSeconds / 60) % 60;
			size_t numRemainder = static_cast<size_t>(numSeconds + 0.5) % 60;
			if(numHours > 0)
				buf << numHours << L"h";
			if (numMinutes > 0)
				buf << setw(2) << setfill(L'0') << numMinutes << L"m";
			buf << setw(2) << setfill(L'0') << numRemainder << "s)";
		}

		if ((flags & FLAG_FIND_OR_CONSOLIDATE_DUPES))
			buf << L"\n# of SHA-256 collisions: " << ctx.numCollisions;
		else
			buf << L"\nBitrate (Mbps): " << Mbps;

		buf << L"\n# of output 'file' records: " << ctx.fileNodeInfoList.size();

		statistics = buf.str();
	}

	if (wantStatistics)
		wcout << statistics << endl;

	// comments
	{
		wostringstream buf;
		buf << L"\nArguments used:\n\t";
		{
			bool looped(false);
			for (int a = 1; a < argc; ++a)
			{
				if (looped)
					buf << L" ";
				buf << argv[a];
				looped = true;
			}
		}

		buf << L"\n" << statistics << endl;

		xmlDoc.insert_child_after(pugi::node_comment, xmlDeclNode)
			.set_value(buf.str().c_str());
	}

	for(auto itFile = ctx.fileNodeInfoList.begin(); itFile != ctx.fileNodeInfoList.end(); ++itFile)
	{
		pugi::xml_node xmlFileNode = xmlRootNode.append_child(L"file");
		xmlFileNode.append_attribute(L"name")
			.set_value(itFile->fileName.c_str());

		xmlFileNode.append_child(L"size")
			.append_child(pugi::node_pcdata)
			.set_value(to_wstring(itFile->fileSize).c_str());

		if (flags & FLAG_MD5)
		{
			pugi::xml_node xmlHashNode = xmlFileNode.append_child(L"hash");
			xmlHashNode.append_attribute(L"type").set_value(L"md5");

			xmlHashNode.append_child(pugi::node_pcdata)
				.set_value(itFile->md5HashStr.c_str());
		}

		if (flags & FLAG_SHA1)
		{
			pugi::xml_node xmlHashNode = xmlFileNode.append_child(L"hash");
			xmlHashNode.append_attribute(L"type").set_value(L"sha-1");

			xmlHashNode.append_child(pugi::node_pcdata)
				.set_value(itFile->sha1HashStr.c_str());
		}

		if (flags & FLAG_SHA256)
		{
			pugi::xml_node xmlHashNode = xmlFileNode.append_child(L"hash");
			xmlHashNode.append_attribute(L"type").set_value(L"sha-256");

			xmlHashNode.append_child(pugi::node_pcdata)
				.set_value(itFile->sha256HashStr.c_str());
		} // end SHA-256

		{
			for (auto itUrl = itFile->urlMap.get()->begin(); itUrl != itFile->urlMap.get()->end(); ++itUrl)
			{
				pugi::xml_node xmlUrlNode = xmlFileNode.append_child(L"url");
				if (!country.empty())
					xmlUrlNode.append_attribute(L"location").set_value(country.c_str());

				xmlUrlNode.append_child(pugi::node_pcdata)
					.set_value(url_encode(itUrl->first, itUrl->second).c_str());
			}
		}
	} // end iterating through all files

	if (!xmlDoc.save_file(outFileName.c_str(), PUGIXML_TEXT("\t"),
		pugi::format_default | pugi::format_write_bom
		| pugi::format_save_file_text,
		pugi::encoding_utf8))
	{
		wcerr << L"Unable to write to file \"" << outFileName << L"\"!";
		return EXIT_FAILURE;
	}

    return EXIT_SUCCESS;
}

