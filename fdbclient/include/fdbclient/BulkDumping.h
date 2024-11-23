/*
 * BulkDump.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2024 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_BULKDUMPING_H
#define FDBCLIENT_BULKDUMPING_H
#include "flow/Trace.h"
#pragma once

#include "fdbclient/FDBTypes.h"
#include "fdbrpc/fdbrpc.h"

enum class BulkDumpPhase : uint8_t {
	Invalid = 0,
	Submitted = 1,
	Complete = 2,
};

enum class BulkDumpFileType : uint8_t {
	Invalid = 0,
	SST = 1,
};

enum class BulkDumpTransportMethod : uint8_t {
	Invalid = 0,
	CP = 1,
};

enum class BulkDumpExportMethod : uint8_t {
	Invalid = 0,
	File = 1,
};

// Definition of bulkdump files metadata
struct BulkDumpFileSet {
	constexpr static FileIdentifier file_identifier = 1384500;

	BulkDumpFileSet() = default;

	BulkDumpFileSet(const std::string& rootPath,
	                const std::string& folderPath,
	                const std::string& manifestPath,
	                const std::string& dataPath,
	                const std::string& byteSamplePath)
	  : rootPath(rootPath), folderPath(folderPath), manifestPath(manifestPath), dataPath(dataPath),
	    byteSamplePath(byteSamplePath) {
		ASSERT(isValid());
	}

	bool isValid() const {
		if (manifestPath.size() == 0) {
			return false;
		}
		if (dataPath.size() == 0 && byteSamplePath.size() > 0) {
			return false;
		}
		return true;
	}

	std::string toString() const {
		return "[RootPath]: " + rootPath + ", [FolderPath]: " + folderPath + ", [ManifestPath]: " + manifestPath +
		       ", [DataPath]: " + dataPath + ", [ByteSamplePath]: " + byteSamplePath;
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar, rootPath, folderPath, manifestPath, dataPath, byteSamplePath);
	}

	std::string rootPath = "";
	std::string folderPath = "";
	std::string manifestPath = "";
	std::string dataPath = "";
	std::string byteSamplePath = "";
};

// Definition of bulkdump metadata
struct BulkDumpState {
	constexpr static FileIdentifier file_identifier = 1384498;

	BulkDumpState() = default;

	// The only public interface to create a valid task
	// This constructor is call when users submitting a task, e.g. by newBulkDumpTaskLocalSST()
	BulkDumpState(KeyRange range,
	              BulkDumpFileType fileType,
	              BulkDumpTransportMethod transportMethod,
	              BulkDumpExportMethod exportMethod,
	              std::string remoteRoot)
	  : jobId(deterministicRandom()->randomUniqueID()), range(range), fileType(fileType),
	    transportMethod(transportMethod), exportMethod(exportMethod), remoteRoot(remoteRoot),
	    phase(BulkDumpPhase::Submitted) {
		ASSERT(isValid());
	}

	bool operator==(const BulkDumpState& rhs) const {
		return jobId == rhs.jobId && taskId == rhs.taskId && range == rhs.range && remoteRoot == rhs.remoteRoot;
	}

	std::string toString() const {
		std::string res = "BulkDumpState: [Range]: " + Traceable<KeyRangeRef>::toString(range) +
		                  ", [FileType]: " + std::to_string(static_cast<uint8_t>(fileType)) +
		                  ", [TransportMethod]: " + std::to_string(static_cast<uint8_t>(transportMethod)) +
		                  ", [ExportMethod]: " + std::to_string(static_cast<uint8_t>(exportMethod)) +
		                  ", [Phase]: " + std::to_string(static_cast<uint8_t>(phase)) +
		                  ", [RemoteRoot]: " + remoteRoot + ", [JobId]: " + jobId.toString();
		if (taskId.present()) {
			res = res + ", [TaskId]: " + taskId.get().toString();
		}
		if (version.present()) {
			res = res + ", [Version]: " + std::to_string(version.get());
		}
		if (remoteFileSet.present()) {
			res = res + ", [RemoteFileSet]: " + remoteFileSet.get().toString();
		}
		return res;
	}

	KeyRange getRange() const { return range; }

	UID getJobId() const { return jobId; }

	Optional<UID> getTaskId() const { return taskId; }

	std::string getRemoteRoot() const { return remoteRoot; }

	BulkDumpPhase getPhase() const { return phase; }

	BulkDumpTransportMethod getTransportMethod() const { return transportMethod; }

	bool isValid() const {
		if (!jobId.isValid()) {
			return false;
		}
		if (taskId.present() && !taskId.get().isValid()) {
			return false;
		}
		if (range.empty()) {
			return false;
		}
		if (transportMethod == BulkDumpTransportMethod::Invalid) {
			return false;
		} else if (transportMethod != BulkDumpTransportMethod::CP) {
			throw not_implemented();
		}
		if (exportMethod == BulkDumpExportMethod::Invalid) {
			return false;
		} else if (exportMethod != BulkDumpExportMethod::File) {
			throw not_implemented();
		}
		if (remoteRoot.empty()) {
			return false;
		}
		return true;
	}

	// The user job spawns a series of ranges tasks based on shard boundary to cover the user task range.
	// Those spawned tasks are executed by SSes.
	// Return metadata of the task.
	BulkDumpState getRangeTaskState(const KeyRange& taskRange) {
		ASSERT(range.contains(taskRange));
		BulkDumpState res = *this; // the task inherits configuration from the job
		UID newTaskId;
		// Guarantee to have a brand new taskId for the new spawned task
		int retryCount = 0;
		while (true) {
			newTaskId = deterministicRandom()->randomUniqueID();
			if (!res.taskId.present() || res.taskId.get() != newTaskId) {
				break;
			}
			retryCount++;
			if (retryCount > 50) {
				TraceEvent(SevError, "GetRangeTaskStateRetryTooManyTimes").detail("TaskRange", taskRange);
				throw bulkdump_task_failed();
			}
		}
		res.taskId = newTaskId;
		res.range = taskRange;
		return res;
	}

	// Generate a metadata with Complete state.
	BulkDumpState getRangeCompleteState(const KeyRange& completeRange, const BulkDumpFileSet& remoteFileSet) {
		ASSERT(range.contains(completeRange));
		ASSERT(remoteFileSet.isValid());
		ASSERT(taskId.present() && taskId.get().isValid());
		BulkDumpState res = *this;
		res.phase = BulkDumpPhase::Complete;
		res.remoteFileSet = remoteFileSet;
		return res;
	}

	template <class Ar>
	void serialize(Ar& ar) {
		serializer(ar,
		           jobId,
		           range,
		           fileType,
		           transportMethod,
		           exportMethod,
		           remoteRoot,
		           phase,
		           taskId,
		           version,
		           remoteFileSet);
	}

private:
	UID jobId; // The unique identifier of a job. Set by user. Any task spawned by the job shares the same jobId and
	           // configuration.

	// File dump config:
	KeyRange range; // Dump the key-value within this range "[begin, end)" from data file
	BulkDumpFileType fileType = BulkDumpFileType::Invalid;
	BulkDumpTransportMethod transportMethod = BulkDumpTransportMethod::Invalid;
	BulkDumpExportMethod exportMethod = BulkDumpExportMethod::Invalid;
	std::string remoteRoot; // remoteRoot is the root string to where the data is set to be uploaded

	// Task dynamics:
	BulkDumpPhase phase = BulkDumpPhase::Invalid;
	Optional<UID> taskId; // The unique identifier of a task. Any SS can do a task. If a task is failed, this remaining
	                      // part of the task can be picked up by any SS with a changed taskId.
	Optional<Version> version;
	Optional<BulkDumpFileSet> remoteFileSet; // Resulting remote filepaths after the dumping task completes
};

// User API to create bulkDump task metadata
// The dumped data is within the input range
// The data is dumped to the input remoteRoot
// The remoteRoot can be either a local root or a remote blobstore root string
BulkDumpState newBulkDumpTaskLocalSST(const KeyRange& range, const std::string& remoteRoot);

#endif
