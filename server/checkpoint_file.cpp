/*
	Copyright (c) 2017 TOSHIBA Digital Solutions Corporation

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU Affero General Public License as
	published by the Free Software Foundation, either version 3 of the
	License, or (at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Affero General Public License for more details.

	You should have received a copy of the GNU Affero General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*!
	@file
	@brief Implementation of CheckpointFile
*/

#include "util/trace.h"
#include "config_table.h"
#include "checkpoint_file.h"
#include <algorithm>
#ifndef _WIN32
#include <fcntl.h>
#include <linux/falloc.h>
#endif

UTIL_TRACER_DECLARE(IO_MONITOR);

const char8_t *const CheckpointFile::gsCpFileBaseName = "gs_cp_";
const char8_t *const CheckpointFile::gsCpFileExtension = ".dat";
const char8_t *const CheckpointFile::gsCpFileSeparator = "_";

#define EXEC_FAILURE(errorNo)

/*!
	@brief Constructore of CheckpointFile.
*/
CheckpointFile::CheckpointFile(
		uint8_t chunkExpSize, const std::string &dir, PartitionGroupId pgId,
		uint32_t splitCount, uint32_t stripeSize,
		const std::vector<std::string> configDirList)
	: BLOCK_EXP_SIZE_(chunkExpSize),
	  BLOCK_SIZE_(static_cast<uint64_t>(1ULL << chunkExpSize)),
	  usedChunkInfo_(10240),
	  validChunkInfo_(10240),
	  blockNum_(0),
	  freeUseBitNum_(0),
	  freeBlockSearchCursor_(0),
	  pgId_(pgId),
	  dir_(dir),
	  splitMode_(splitCount > 0),
	  splitCount_(splitCount ? splitCount : 1),
	  stripeSize_(stripeSize),
	  readBlockCount_(0),
	  writeBlockCount_(0),
	  readRetryCount_(0),
	  writeRetryCount_(0),
	  ioWarningThresholdMillis_(IO_MONITOR_DEFAULT_WARNING_THRESHOLD_MILLIS) {

	try {
		fileList_.assign(splitCount_, NULL);
		dirList_.assign(splitCount_, "");
		fileNameList_.assign(splitCount_, "");
		blockCountList_.assign(splitCount_, 0);
		size_t configDirCount = configDirList.size();
		if (splitMode_) {
			if (configDirCount > splitCount_) {
				GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
						"Config error: dbFilePathList.size() > dbFileSplitCount");
			}
			if (configDirCount == 0) {
				GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
						"Config error: dbFilePathList is empty");
			}
			if (splitCount_ > FILE_SPLIT_COUNT_LIMIT) {
				GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
						"Config error: dbFileSplitCount is too large");
			}
			if (stripeSize_ > FILE_SPLIT_STRIPE_SIZE_LIMIT) {
				GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
						"Config error: dbFileSplitStripeSize is too large");
			}
			for (uint32_t i = 0; i < splitCount_; ++i) {
				dirList_[i] = configDirList[i % configDirCount];
				if (util::FileSystem::exists(dirList_[i].c_str())) {
					if (!util::FileSystem::isDirectory(dirList_[i].c_str())) {
						GS_THROW_SYSTEM_ERROR(
							GS_ERROR_CM_IO_ERROR, "Specified file path is exist, but path=\""
							<< dirList_[i].c_str() << "\" is not directory");
					}
				}
			}
		}
		else {
			if (configDirCount != 0) {
				GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
						"dbFilePathList.size() > 0, but dbFileSplitCount == 0");
			}
			dirList_[0] = dir;
		}
	} catch(std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Invalid configure found.");
	}
}

/*!
	@brief Destructor of CheckpointFile.
*/
CheckpointFile::~CheckpointFile() {
	for (uint32_t i = 0; i < splitCount_; ++i) {
		try {
			if (fileList_[i] && !fileList_[i]->isClosed()) {
				fileList_[i]->unlock();
				fileList_[i]->close();
			}
			delete fileList_[i];
			fileList_[i] = NULL;
		} catch(...) {
			;
		}
	}
}

/*!
	@brief Open a file.
*/
bool CheckpointFile::open(bool checkOnly, bool createMode) {
	try {
		assert(splitCount_ > 0);
		int64_t totalBlockCount = 0;
		for (uint32_t i = 0; i < splitCount_; ++i) {
			util::NormalOStringStream ss;
			util::NormalOStringStream ssFile;
			if (splitMode_) {
				if (util::FileSystem::exists(dirList_[i].c_str())) {
					if (!util::FileSystem::isDirectory(dirList_[i].c_str())) {
						GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
								"Not direcotry: directoryName=" << dirList_[i].c_str());
					}
				}
				else {
					GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
							"Directory not found: directoryName=" << dirList_[i].c_str());
				}
				ss << dirList_[i] << "/";
				ss << CheckpointFile::gsCpFileBaseName << pgId_
				  << CheckpointFile::gsCpFileSeparator;
				ssFile << ss.str() << i << CheckpointFile::gsCpFileExtension;
			}
			else {
				assert(!splitMode_);
				assert(splitCount_ == 1);
				if (!dir_.empty()) {
					if (util::FileSystem::exists(dir_.c_str())) {
						if (!util::FileSystem::isDirectory(dir_.c_str())) {
							GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
									"Not direcotry: directoryName=" << dir_.c_str());
						}
					}
					else {
						GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_INVALID_DIRECTORY,
							"Directory not found: directoryName=" << dir_.c_str());
					}
					ss << dir_ << "/";
				}
				ss << CheckpointFile::gsCpFileBaseName << pgId_
				  << CheckpointFile::gsCpFileSeparator;
				ssFile << ss.str() << 1 << CheckpointFile::gsCpFileExtension;
			}
			fileNameList_[i].assign(ssFile.str());

			if (util::FileSystem::exists(fileNameList_[i].c_str())) {
				if (fileList_[i]) {
					delete fileList_[i];
					fileList_[i] = NULL;
				}
				fileList_[i] = UTIL_NEW util::NamedFile();
				fileList_[i]->open(fileNameList_[i].c_str(),
							(checkOnly ? util::FileFlag::TYPE_READ_ONLY
						 : util::FileFlag::TYPE_READ_WRITE));
#ifndef _WIN32 
				if (!checkOnly) {
					fileList_[i]->lock();
				}
#endif
				util::FileStatus status;
				fileList_[i]->getStatus(&status);
				int64_t chunkNum =
				  (status.getSize() + BLOCK_SIZE_ - 1) / BLOCK_SIZE_;
				blockCountList_[i] = chunkNum;
				totalBlockCount += chunkNum;
			}
			else {
				if (checkOnly) {
					GS_THROW_USER_ERROR(GS_ERROR_CM_FILE_NOT_FOUND,
										"Checkpoint file not found despite check only.");
				}
				if (!createMode) {
					GS_THROW_USER_ERROR(
						GS_ERROR_CM_FILE_NOT_FOUND, "Checkpoint file not found.");
				}
				
				fileList_[i] = UTIL_NEW util::NamedFile();
				fileList_[i]->open(fileNameList_[i].c_str(),
							util::FileFlag::TYPE_CREATE | util::FileFlag::TYPE_READ_WRITE);
#ifndef _WIN32 
				fileList_[i]->lock();
#endif
				blockCountList_[i] = 0;
			}
		} 
		blockNum_ = totalBlockCount;
		usedChunkInfo_.reserve(totalBlockCount + 1);
		usedChunkInfo_.set(totalBlockCount + 1, false);
		validChunkInfo_.reserve(totalBlockCount + 1);
		validChunkInfo_.set(totalBlockCount + 1, false);
		freeUseBitNum_ = usedChunkInfo_.length();  
		freeBlockSearchCursor_ = 0;
		assert(freeUseBitNum_ <= usedChunkInfo_.length());
		return (totalBlockCount == 0); 
	}
	catch (SystemException &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file open failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file open failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
}

/*!
	@brief Truncate a file.
*/
void CheckpointFile::truncate() {
	try {
		for (uint32_t i = 0; i < splitCount_; ++i) {
			if (fileList_[i]) {
				delete fileList_[i];
			}
			fileList_[i] = UTIL_NEW util::NamedFile();
			fileList_[i]->open(fileNameList_[i].c_str(), util::FileFlag::TYPE_CREATE |
							   util::FileFlag::TYPE_TRUNCATE |
							   util::FileFlag::TYPE_READ_WRITE);
			UTIL_TRACE_WARNING(CHECKPOINT_FILE, "file truncated.");
#ifndef _WIN32 
			fileList_[i]->lock();
#endif
			blockCountList_[i] = 0;
		}
		blockNum_ = 0;
		freeUseBitNum_ = 0;
		freeBlockSearchCursor_ = 0;
		usedChunkInfo_.reset();
		validChunkInfo_.reset();
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file truncate failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
}

void CheckpointFile::advise(int32_t advise) {
#ifndef WIN32
	for (uint32_t i = 0; i < splitCount_; ++i) {
		if (fileList_[i] && !fileList_[i]->isClosed()) {
			int32_t ret = posix_fadvise(fileList_[i]->getHandle(), 0, 0, advise);
			if (ret > 0) {
				UTIL_TRACE_WARNING(CHECKPOINT_FILE,
								   "fadvise failed. :" <<
								   "cpFile_pgId," << pgId_ <<
								   ",advise," << advise <<
								   ",returnCode," << ret);
			}
			UTIL_TRACE_INFO(CHECKPOINT_FILE,
							"advise(POSIX_FADV_DONTNEED) : cpFile_pgId," << pgId_);
		}
	}
#endif
}

/*!
	@brief Allocate a chunkSize-block on the checkpoint file.
*/
int64_t CheckpointFile::allocateBlock() {
	int64_t allocatePos = -1;

	int32_t count = 0;
	uint64_t pos = freeBlockSearchCursor_;
	uint64_t usedChunkInfoSize = usedChunkInfo_.length();

	if (freeUseBitNum_ > 0) {
		uint64_t startPos = freeBlockSearchCursor_;
		for (pos = freeBlockSearchCursor_; pos < usedChunkInfoSize;
			 ++pos, ++count) {
			if (!usedChunkInfo_.get(pos)) {
				allocatePos = pos;
				break;
			}
			if (count > ALLOCATE_BLOCK_SEARCH_LIMIT) {
				break;
			}
		}
		if (allocatePos == -1 && count <= ALLOCATE_BLOCK_SEARCH_LIMIT) {
			for (pos = 0; pos < startPos; ++pos, ++count) {
				if (!usedChunkInfo_.get(pos)) {
					allocatePos = pos;
					break;
				}
				if (count > ALLOCATE_BLOCK_SEARCH_LIMIT) {
					break;
				}
			}
		}
		freeBlockSearchCursor_ = pos + 1;
		if (freeBlockSearchCursor_ >= usedChunkInfoSize) {
			freeBlockSearchCursor_ = 0;
		}
	}
	if (allocatePos == -1) {
		allocatePos = usedChunkInfo_.append(true);
		assert(allocatePos != -1);
		validChunkInfo_.set(allocatePos, false);
		UTIL_TRACE_INFO(CHECKPOINT_FILE, "allocateBlock(NEW): " << allocatePos);
	}
	else {
		UTIL_TRACE_INFO(
			CHECKPOINT_FILE, "allocateBlock(reuse): " << allocatePos);
	}
	setUsedBlockInfo(allocatePos, true);
	return allocatePos;
}

/*!
	@brief Free an allocated chunkSize-block on the checkpoint file.
*/
void CheckpointFile::freeBlock(uint64_t blockNo) {
	UTIL_TRACE_INFO(CHECKPOINT_FILE, "freeBlock: " << blockNo);
	assert(usedChunkInfo_.length() >= (blockNo));
	assert(usedChunkInfo_.get(blockNo));

	setUsedBlockInfo(blockNo, false);
	UTIL_TRACE_INFO(CHECKPOINT_FILE, "freeBlock: " << blockNo);
}

/*!
	@brief Set a flag of the used block.
*/
void CheckpointFile::setUsedBlockInfo(uint64_t blockNo, bool flag) {
	UTIL_TRACE_DEBUG(
		CHECKPOINT_FILE, "setUsedBlock: " << blockNo << ",val," << flag);

	bool oldFlag = usedChunkInfo_.get(blockNo);
	usedChunkInfo_.set(blockNo, flag);
	if (flag && (oldFlag != flag)) {
		assert(freeUseBitNum_ != 0);
		--freeUseBitNum_;
	}
	else if (!flag && (oldFlag != flag)) {
		++freeUseBitNum_;
	}
	assert(freeUseBitNum_ <= usedChunkInfo_.length());
}

/*!
	@brief Return a flag of the used block.
*/
bool CheckpointFile::getUsedBlockInfo(uint64_t blockNo) {
	assert(usedChunkInfo_.length() >= blockNo);
	return usedChunkInfo_.get(blockNo);
}

/*!
	@brief Initialize a flag of the used block.
*/
void CheckpointFile::initializeUsedBlockInfo() {
	usedChunkInfo_.clear();
	usedChunkInfo_.reserve(blockNum_);
	freeUseBitNum_ = blockNum_;
	for (uint64_t pos = 0; pos < blockNum_; ++pos) {
		usedChunkInfo_.append(false);
	}
	assert(freeUseBitNum_ <= usedChunkInfo_.length());
}

/*!
	@brief Set a flag of the recent checkpoint block.
*/
void CheckpointFile::setValidBlockInfo(uint64_t blockNo, bool flag) {
	validChunkInfo_.set(blockNo, flag);
}

/*!
	@brief Return a flag of the recent checkpoint block.
*/
bool CheckpointFile::getValidBlockInfo(uint64_t blockNo) {
	return validChunkInfo_.get(blockNo);
}

/*!
	@brief Initialize a flag of the used block.
*/
void CheckpointFile::initializeValidBlockInfo() {
	validChunkInfo_.clear();
	validChunkInfo_.reserve(blockNum_);
	for (uint64_t pos = 0; pos < blockNum_; ++pos) {
		validChunkInfo_.append(false);
	}
}

/*!
	@brief Write chunkSize-block.
*/
void CheckpointFile::punchHoleBlock(uint32_t size, uint64_t offset) {

#ifdef _WIN32
#else
	const uint64_t startClock =
		util::Stopwatch::currentClock();  
	size_t fileNth = calcFileNth(offset);
	uint64_t fileOffset = calcFileOffset(offset);
	try {
		if (fileList_[fileNth] && !fileList_[fileNth]->isClosed() && 0 < size) {
			fileList_[fileNth]->preAllocate(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
				fileOffset, size);
		}
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file fallocate failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << fileNameList_[fileNth]
									   << ",pgId," << pgId_ 
									   << ",offset," << fileOffset
									   << ",size," << size << ")");
	}

	const uint32_t lap = util::Stopwatch::clockToMillis(
		util::Stopwatch::currentClock() - startClock);
	if (lap > ioWarningThresholdMillis_) {  
		UTIL_TRACE_WARNING(
			IO_MONITOR, "[LONG I/O] punching hole time,"
							<< lap << ",fileName," << fileNameList_[fileNth] << ",pgId,"
							<< pgId_ << ",offset," << fileOffset
							<< ",size," << size
							<< ",writeBlockCount_=" << writeBlockCount_);
	}
#endif
}

void CheckpointFile::zerofillUnusedBlock() {
#ifdef _WIN32
#else


	uint64_t headBlockId = 0;
	off_t offset = 0;
	off_t length = 0;
	size_t punchCount = 0;
	size_t totalCount = 0;
	uint64_t fileNth = 0;
	uint64_t fileOffset = 0;

	const uint64_t blockNum = usedChunkInfo_.length();
	const uint64_t startClock = util::Stopwatch::currentClock();
	try {
		for (uint64_t i = 1; i < blockNum; ++i) {
			if (!usedChunkInfo_.get(i)) {
				offset = i * BLOCK_SIZE_;
				fileNth = calcFileNth(offset);
				fileOffset = calcFileOffset(offset);
				assert(fileList_[fileNth]);

				length = BLOCK_SIZE_;
				fileList_[fileNth]->preAllocate(
						FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
						fileOffset, length);
				++totalCount;
				++punchCount;
			}
		}
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e,
				"Punching holes in checkpoint file has failed. (reason="
				<< GS_EXCEPTION_MESSAGE(e) << fileNameList_[fileNth]
				<< ",pgId," << pgId_
				<< ",offset," << fileOffset
				<< ",size," << length << ")");
	}

	const uint32_t lap = util::Stopwatch::clockToMillis(
			util::Stopwatch::currentClock() - startClock);
	UTIL_TRACE_INFO(
			IO_MONITOR, "Punching hole time," << lap
			<< ",fileName," << fileNameList_[0] << ",pgId,"
			<< pgId_ << ",holePunchCount," << punchCount
			<< ",holeBlockCount," << totalCount
			<< ",totalBlockCount," << blockNum);
#endif 
}

/*!
	@brief Write chunkSize-block.
*/
int64_t CheckpointFile::writeBlock(
	const uint8_t *buffer, uint32_t size, uint64_t blockNo) {
	assert(size != 0);
	uint64_t offset = blockNo * BLOCK_SIZE_;
	size_t fileNth = calcFileNth(offset);
	uint64_t fileOffset = calcFileOffset(offset);
	try {
		if (!fileList_[fileNth]) {
			fileList_[fileNth] = UTIL_NEW util::NamedFile();
			fileList_[fileNth]->open(fileNameList_[fileNth].c_str(),
				util::FileFlag::TYPE_CREATE | util::FileFlag::TYPE_READ_WRITE);
#ifndef _WIN32 
			fileList_[fileNth]->lock();
#endif
		}
		uint64_t retryCount = 0;
		GS_FILE_WRITE_ALL(
				IO_MONITOR, (*fileList_[fileNth]), buffer,
				(size << BLOCK_EXP_SIZE_), fileOffset,
				ioWarningThresholdMillis_, retryCount);
		ssize_t writtenSize = size << BLOCK_EXP_SIZE_;
		writeRetryCount_ += retryCount;
		if (blockNum_ < (size + blockNo)) {
			blockNum_ = (size + blockNo);
			UTIL_TRACE_INFO(
				CHECKPOINT_FILE, fileNameList_[0] + " extended. File size = "
									 << (blockNo + size) 
									 << ",blockNum_=" << blockNum_);
		}

		uint64_t writeBlockNum = (writtenSize >> BLOCK_EXP_SIZE_);
		writeBlockCount_ += writeBlockNum;
		UTIL_TRACE_INFO(CHECKPOINT_FILE,
			fileNameList_[fileNth] << ",blockNo," << blockNo
					  << ",writeBlockCount=" << writeBlockCount_);
		return size;
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file write failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << fileNameList_[fileNth]
									   << ",pgId," << pgId_ << ",blockNo,"
									   << blockNo << ")");
	}
}

/*!
	@brief Write byteSize-data.
*/
int64_t CheckpointFile::writePartialBlock(
	const uint8_t *buffer, uint32_t size, uint64_t offset) {
	size_t fileNth = calcFileNth(offset);
	uint64_t fileOffset = calcFileOffset(offset);
	try {
		if (!fileList_[fileNth]) {
			fileList_[fileNth] = UTIL_NEW util::NamedFile();
			fileList_[fileNth]->open(fileNameList_[fileNth].c_str(),
				util::FileFlag::TYPE_CREATE | util::FileFlag::TYPE_READ_WRITE);
#ifndef _WIN32 
			fileList_[fileNth]->lock();
#endif
		}

		uint64_t retryCount = 0;
		GS_FILE_WRITE_ALL(
				IO_MONITOR, (*fileList_[fileNth]), buffer, size, fileOffset,
				ioWarningThresholdMillis_, retryCount);
		ssize_t writtenSize = size;
		writeRetryCount_ += retryCount;
		if ((blockNum_ << BLOCK_EXP_SIZE_) < size + offset) {
			blockNum_ = ((size + offset + BLOCK_SIZE_ - 1) >> BLOCK_EXP_SIZE_);
			UTIL_TRACE_INFO(
				CHECKPOINT_FILE, fileNameList_[0] + " extended. File size = "
									 << (size + offset) << "(Byte)"
									 << ",blockNum_=" << blockNum_);
		}

		UTIL_TRACE_INFO(CHECKPOINT_FILE,
			fileNameList_[fileNth] << ",write, offset," << fileOffset << ",size=" << size);

		return writtenSize;
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file write failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
}

/*!
	@brief Read chunkSize-block.
*/
int64_t CheckpointFile::readBlock(
	uint8_t *buffer, uint32_t size, uint64_t blockNo) {

	assert(size != 0);
	if (blockNum_ < size + blockNo - 1 || size == 0) {
		GS_THROW_SYSTEM_ERROR(GS_ERROR_CF_READ_CHUNK_FAILED, 
			"Checkpoint file read failed. (reason= invalid parameter."
			<< " size = " << size << ", blockNo = " << blockNo
			<< ", blockNum = " << blockNum_ << ")");
	}

	uint64_t offset = blockNo * BLOCK_SIZE_;
	size_t fileNth = calcFileNth(offset);
	uint64_t fileOffset = calcFileOffset(offset);
	try {
		if (!fileList_[fileNth]) {
			if (util::FileSystem::exists(fileNameList_[fileNth].c_str())) {
				fileList_[fileNth] = UTIL_NEW util::NamedFile();
				fileList_[fileNth]->open(fileNameList_[fileNth].c_str(), util::FileFlag::TYPE_READ_WRITE);
#ifndef _WIN32 
				fileList_[fileNth]->lock();
#endif
			}
			else {
				return 0;
			}
		}
		uint64_t retryCount = 0;
		GS_FILE_READ_ALL(
				IO_MONITOR, (*fileList_[fileNth]), buffer,
				(size << BLOCK_EXP_SIZE_), fileOffset,
				ioWarningThresholdMillis_, retryCount);
		ssize_t readSize = size << BLOCK_EXP_SIZE_;
		readRetryCount_ += retryCount;
		int64_t readBlockNum = (readSize >> BLOCK_EXP_SIZE_);
		readBlockCount_ += readBlockNum;
		UTIL_TRACE_INFO(CHECKPOINT_FILE,
			fileList_[fileNth] << ",blockNo," << blockNo
					  << ",readBlockCount_=" << readBlockCount_);
		return readBlockNum;
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file read failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
}


/*!
	@brief Return file size.
*/
int64_t CheckpointFile::getFileSize() {
	int64_t totalFileSize = 0;
	try {
		for (uint32_t i = 0; i < splitCount_; ++i) {
			util::FileStatus fileStatus;
			if (fileList_[i]) {
				fileList_[i]->getStatus(&fileStatus);
				totalFileSize += fileStatus.getSize();
			}
		}
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file read failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
	return totalFileSize;
}

int64_t CheckpointFile::getSplitFileSize(uint32_t splitId) {
	assert(splitId < splitCount_);
	util::FileStatus fileStatus;
	if (fileList_[splitId]) {
		fileList_[splitId]->getStatus(&fileStatus);
		return fileStatus.getSize();
	}
	else {
		return 0;
	}
}

/*!
	@brief Close the file.
*/
int64_t CheckpointFile::getFileAllocateSize() {
	if (0 < blockNum_) {
#ifdef _WIN32
		return getFileSize();
#else
		int64_t blockSize = 0;
		try {
			for (uint32_t i = 0; i < splitCount_; ++i) {
				util::FileStatus fileStatus;
				if (fileList_[i]) {
					fileList_[i]->getStatus(&fileStatus);
					blockSize += fileStatus.getBlockCount() * 512;
				}
			}
		}
		catch (std::exception &e) {
			GS_RETHROW_SYSTEM_ERROR(e, "Checkpoint file read failed. (reason="
										   << GS_EXCEPTION_MESSAGE(e) << ")");
		}
		return blockSize;
#endif
	}
	return 0;
}

size_t CheckpointFile::getFileSystemBlockSize(const std::string &dir) {
	size_t fileSystemBlockSize;
	try {
		util::FileSystemStatus status;
		util::FileSystem::getStatus(dir.c_str(), &status);
		fileSystemBlockSize = status.getBlockSize();
	}
	catch (std::exception &e) {
		GS_RETHROW_SYSTEM_ERROR(e, "Directory access failed. (reason="
									   << GS_EXCEPTION_MESSAGE(e) << ")");
	}
	return fileSystemBlockSize;
}

size_t CheckpointFile::getFileSystemBlockSize() {
	return getFileSystemBlockSize(dir_);
}

void CheckpointFile::close() {
	for (uint32_t i = 0; i < splitCount_; ++i) {
		if (fileList_[i]) {
			fileList_[i]->close();
			delete fileList_[i];
		}
		fileList_[i] = NULL;
	}
}

/*!
	@brief Flush the file.
*/
void CheckpointFile::flush() {
	for (uint32_t i = 0; i < splitCount_; ++i) {
		if (fileList_[i]) {
			uint64_t startClock =
					util::Stopwatch::currentClock();  
			fileList_[i]->sync();
			uint32_t lap = util::Stopwatch::clockToMillis(
					util::Stopwatch::currentClock() - startClock);  
			if (lap > ioWarningThresholdMillis_) {				
				UTIL_TRACE_WARNING(IO_MONITOR,
						"[LONG I/O] sync time," << lap << ",fileName," << fileNameList_[i]);
			}
		}
	}
}

uint64_t CheckpointFile::getReadBlockCount() {
	return readBlockCount_;
}
uint64_t CheckpointFile::getWriteBlockCount() {
	return writeBlockCount_;
}
void CheckpointFile::resetReadBlockCount() {
	readBlockCount_ = 0;
}
void CheckpointFile::resetWriteBlockCount() {
	writeBlockCount_ = 0;
}

uint64_t CheckpointFile::getReadRetryCount() {
	return readRetryCount_;
}
uint64_t CheckpointFile::getWriteRetryCount() {
	return writeRetryCount_;
}
void CheckpointFile::resetReadRetryCount() {
	readRetryCount_ = 0;
}
void CheckpointFile::resetWriteRetryCount() {
	writeRetryCount_ = 0;
}

/*!
	@brief Return if fileName is of a checkpointFile.
*/
bool CheckpointFile::checkFileName(
	const std::string &fileName, PartitionGroupId &pgId, int32_t &splitId) {
	pgId = UNDEF_PARTITIONGROUPID;
	splitId = -1;

	std::string::size_type pos = fileName.find(gsCpFileBaseName);
	if (0 != pos) {
		return false;
	}
	pos = fileName.rfind(gsCpFileExtension);
	if (std::string::npos == pos) {
		return false;
	}
	else if ((pos + strlen(gsCpFileExtension)) != fileName.length()) {
		return false;
	}

	util::NormalIStringStream iss(fileName);
	iss.seekg(strlen(gsCpFileBaseName));
	uint32_t num1;
	int32_t num2;
	char c;
	iss >> num1 >> c >> num2;

	if (iss.fail()) {
		return false;
	}
	if (c != '_') {
		return false;
	}
	if (static_cast<std::string::size_type>(iss.tellg()) != pos) {
		return false;
	}
	pgId = num1;  
	splitId = num2;
	return true;
}

std::string CheckpointFile::dump() {
	return fileNameList_[0];
}

std::string CheckpointFile::dumpUsedChunkInfo() {
	return usedChunkInfo_.dumpUnit();
}

std::string CheckpointFile::dumpValidChunkInfo() {
	return validChunkInfo_.dumpUnit();
}
