#include "nexus/engine.h"
#include "nexus/usn_record_parser.h"

#include <gtest/gtest.h>
#include <windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>

namespace nexus {
struct EngineJournalTestAccess {
    static void SeedMftRoot(Engine& engine, const std::wstring& volume, const std::wstring& path) {
        std::lock_guard operation_lock(engine.operation_mutex_);
        std::lock_guard lock(engine.mutex_);
        engine.mft_indexed_volumes_.insert(volume);
        FileRecord root;
        root.file_id = 5;
        root.parent_id = 5;
        root.volume_id = volume;
        root.path = path;
        root.is_directory = true;
        engine.AddRecordLocked(std::move(root));
    }

    static JournalReadResult Apply(Engine& engine, const std::wstring& volume,
                                  const std::vector<UsnFileEntry>& entries) {
        std::lock_guard operation_lock(engine.operation_mutex_);
        JournalReadResult result;
        result.state = JournalReadState::Ok;
        result.next_usn = 100;
        return engine.ApplyUsnBatch(volume, entries, std::move(result));
    }

    static void SetCheckpoint(Engine& engine, const std::wstring& volume, std::uint64_t id, std::int64_t usn) {
        std::lock_guard lock(engine.mutex_);
        engine.mft_indexed_volumes_.insert(volume);
        engine.journal_checkpoints_[volume] = {id, usn};
    }
};
}

TEST(NexusEngine, ExactAndPrefixSearchReturnPaths) {
    const auto root = std::filesystem::temp_directory_path() / L"nexus_phase1_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / L"nested");
    std::ofstream(root / L"program.exe").put('x');
    std::ofstream(root / L"programming.py").put('x');
    std::ofstream(root / L"telegram.txt").put('x');
    std::ofstream(root / L"nested" / L"progress.txt").put('x');

    nexus::Engine engine;
    engine.IndexPath(root);
    const auto exact = engine.SearchExact(L"PROGRAM.EXE");
    ASSERT_EQ(exact.size(), 1u);
    EXPECT_EQ(exact.front().path, (root / L"program.exe").wstring());
    EXPECT_NE(exact.front().parent_id, 0u);
    const auto prefix = engine.SearchPrefix(L"prog");
    EXPECT_EQ(prefix.size(), 3u);
    EXPECT_EQ(engine.Stats().files, 4u);
    EXPECT_EQ(engine.SearchSubstring(L"GRAM").size(), 3u);
    EXPECT_EQ(engine.SearchSubstring(L"og").size(), 3u);
    const auto nested = engine.SearchExact(L"nested");
    ASSERT_EQ(nested.size(), 1u);
    EXPECT_TRUE(nested.front().is_directory);
    const auto progress = engine.SearchExact(L"progress.txt");
    ASSERT_EQ(progress.size(), 1u);
    EXPECT_EQ(progress.front().parent_id, nested.front().file_id);

    engine.StartIndexing({root});
    engine.StopIndexing();
    engine.Clear();
    engine.IndexPath(root);
    EXPECT_EQ(engine.SearchPrefix(L"prog").size(), 3u);
    std::filesystem::remove_all(root, ec);
}

TEST(NexusEngine, ReportsInvalidRoot) {
    nexus::Engine engine;
    const auto root = std::filesystem::temp_directory_path() / L"nexus_missing_phase1_root";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    engine.IndexPath(root);
    EXPECT_EQ(engine.Stats().errors, 1u);
    EXPECT_EQ(engine.Stats().files, 0u);
}

TEST(NexusEngine, SearchFiltersApplyBeforeLimitAndRespectMetadata) {
    const auto root = std::filesystem::temp_directory_path() / L"nexus_search_filters_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / L"nested");
    std::ofstream(root / L"alpha-small.txt") << "x";
    std::ofstream(root / L"nested" / L"alpha-large.TXT") << "enough bytes";
    std::ofstream(root / L"nested" / L"alpha-large.bin") << "enough bytes";

    nexus::Engine engine;
    engine.IndexPath(root);
    nexus::SearchFilter filter;
    filter.extension = L".txt";
    filter.path_prefix = (root / L"nested").wstring();
    filter.files_only = true;
    filter.min_size_bytes = 5;
    const auto filtered = engine.SearchFiltered(L"alpha", L"prefix", filter, 1);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].name, L"alpha-large.TXT");

    filter.files_only = false;
    filter.folders_only = true;
    EXPECT_TRUE(engine.SearchFiltered(L"alpha", L"prefix", filter, 10).empty());
    filter.folders_only = false;
    filter.max_size_bytes = 1;
    EXPECT_TRUE(engine.SearchFiltered(L"alpha", L"prefix", filter, 10).empty());
    std::filesystem::remove_all(root, ec);
}

TEST(UsnParser, ReadsV2RecordAndContinuation) {
    const std::wstring name = L"café.txt";
    const auto name_bytes = name.size() * sizeof(WCHAR);
    const auto record_size = offsetof(USN_RECORD_V2, FileName) + name_bytes;
    std::vector<std::byte> bytes(sizeof(ULONGLONG) + record_size);
    const ULONGLONG continuation = 0x12345678;
    std::memcpy(bytes.data(), &continuation, sizeof(continuation));
    USN_RECORD_V2 record{};
    record.RecordLength = static_cast<DWORD>(record_size);
    record.MajorVersion = 2;
    record.FileReferenceNumber = 17;
    record.ParentFileReferenceNumber = 5;
    record.TimeStamp.QuadPart = 99;
    record.FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    record.FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
    record.FileNameLength = static_cast<WORD>(name_bytes);
    std::memcpy(bytes.data() + sizeof(ULONGLONG), &record, offsetof(USN_RECORD_V2, FileName));
    std::memcpy(bytes.data() + sizeof(ULONGLONG) + record.FileNameOffset, name.data(), name_bytes);

    nexus::UsnEnumBatch batch;
    std::wstring error;
    ASSERT_TRUE(nexus::ParseUsnEnumBuffer(bytes, batch, &error)) << "USN V2 buffer should parse";
    ASSERT_EQ(batch.entries.size(), 1u);
    EXPECT_EQ(batch.continuation_value, continuation);
    EXPECT_EQ(batch.entries[0].file_reference, 17u);
    EXPECT_EQ(batch.entries[0].parent_reference, 5u);
    EXPECT_EQ(batch.entries[0].name, name);
}

TEST(UsnParser, RejectsOutOfBoundsRecordWithoutChangingOutput) {
    std::vector<std::byte> bytes(sizeof(ULONGLONG) + offsetof(USN_RECORD_V2, FileName));
    const DWORD bad_length = (std::numeric_limits<DWORD>::max)();
    std::memcpy(bytes.data() + sizeof(ULONGLONG), &bad_length, sizeof(bad_length));
    nexus::UsnEnumBatch batch;
    batch.continuation_value = 7;
    batch.entries.push_back({.file_reference = 99});
    EXPECT_FALSE(nexus::ParseUsnEnumBuffer(bytes, batch));
    EXPECT_EQ(batch.continuation_value, 7u);
    ASSERT_EQ(batch.entries.size(), 1u);
    EXPECT_EQ(batch.entries[0].file_reference, 99u);
}

TEST(NexusEngine, JournalSyncRequiresStableMftIdentityMap) {
    nexus::Engine engine;
    const auto result = engine.SyncUsnJournal(L"C:\\", 0, 0);
    EXPECT_EQ(result.state, nexus::JournalReadState::ReconciliationRequired);
    EXPECT_FALSE(result.applied);
    EXPECT_EQ(result.next_usn, 0);
}

TEST(UsnParser, CoalescesDuplicateFileEventsWithoutLosingReasons) {
    nexus::UsnFileEntry first;
    first.file_reference = 21;
    first.usn = 10;
    first.reason = USN_REASON_FILE_CREATE;
    first.name = L"new.txt";
    nexus::UsnFileEntry second = first;
    second.usn = 15;
    second.reason = USN_REASON_DATA_EXTEND;
    second.attributes = FILE_ATTRIBUTE_ARCHIVE;
    const auto events = nexus::CoalesceUsnEvents({first, second});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].usn, 15);
    EXPECT_EQ(events[0].reason, USN_REASON_FILE_CREATE | USN_REASON_DATA_EXTEND);
    EXPECT_EQ(events[0].attributes, FILE_ATTRIBUTE_ARCHIVE);
}

TEST(UsnJournal, DetectsDeletedRangeAndAcceptsFreshCheckpointAfterReconciliation) {
    EXPECT_EQ(nexus::ValidateJournalCheckpoint(4, 4, 10, 20, 30),
              nexus::JournalReadState::ReconciliationRequired);
    EXPECT_EQ(nexus::ClassifyJournalReadError(ERROR_JOURNAL_ENTRY_DELETED),
              nexus::JournalReadState::ReconciliationRequired);
    EXPECT_EQ(nexus::ValidateJournalCheckpoint(0, 5, 40, 20, 40),
              nexus::JournalReadState::Ok);
}

TEST(NexusEngine, JournalCreateUpdateAndDuplicateDeleteAreIncremental) {
    const auto root = std::filesystem::temp_directory_path() / L"nexus_usn_apply_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directory(root);
    const auto file_path = root / L"created.txt";
    std::ofstream(file_path) << "a";

    nexus::Engine engine;
    const std::wstring volume = L"C:\\";
    nexus::EngineJournalTestAccess::SeedMftRoot(engine, volume, root.wstring());
    nexus::UsnFileEntry created;
    created.file_reference = 42;
    created.parent_reference = 5;
    created.usn = 10;
    created.reason = USN_REASON_FILE_CREATE | USN_REASON_CLOSE;
    created.attributes = FILE_ATTRIBUTE_NORMAL;
    created.name = L"created.txt";
    auto applied = nexus::EngineJournalTestAccess::Apply(engine, volume, {created, created});
    ASSERT_EQ(applied.state, nexus::JournalReadState::Ok);
    EXPECT_TRUE(applied.applied);
    EXPECT_EQ(engine.SearchExact(L"created.txt").size(), 1u);

    std::ofstream(file_path, std::ios::app) << "more data";
    auto modified = created;
    modified.usn = 11;
    modified.reason = USN_REASON_DATA_EXTEND | USN_REASON_CLOSE;
    applied = nexus::EngineJournalTestAccess::Apply(engine, volume, {modified});
    ASSERT_EQ(applied.state, nexus::JournalReadState::Ok);
    EXPECT_GT(engine.SearchExact(L"created.txt").front().size_bytes, 1u);

    auto deleted = created;
    deleted.usn = 12;
    deleted.reason = USN_REASON_FILE_DELETE | USN_REASON_CLOSE;
    applied = nexus::EngineJournalTestAccess::Apply(engine, volume, {deleted});
    ASSERT_EQ(applied.state, nexus::JournalReadState::Ok);
    EXPECT_TRUE(engine.SearchExact(L"created.txt").empty());
    applied = nexus::EngineJournalTestAccess::Apply(engine, volume, {deleted});
    EXPECT_EQ(applied.state, nexus::JournalReadState::Ok);
    EXPECT_EQ(applied.applied_changes, 0u);

    std::filesystem::remove_all(root, ec);
}

TEST(NexusEngine, UnsupportedRenameLeavesBatchUnapplied) {
    const auto root = std::filesystem::temp_directory_path() / L"nexus_usn_transaction_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directory(root);
    std::ofstream(root / L"would_be_added.txt") << "x";
    nexus::Engine engine;
    const std::wstring volume = L"C:\\";
    nexus::EngineJournalTestAccess::SeedMftRoot(engine, volume, root.wstring());
    nexus::UsnFileEntry created;
    created.file_reference = 55;
    created.parent_reference = 5;
    created.reason = USN_REASON_FILE_CREATE;
    created.attributes = FILE_ATTRIBUTE_NORMAL;
    created.name = L"would_be_added.txt";
    auto rename = created;
    rename.file_reference = 5;
    rename.reason = USN_REASON_RENAME_OLD_NAME;
    rename.attributes = FILE_ATTRIBUTE_DIRECTORY;
    const auto result = nexus::EngineJournalTestAccess::Apply(engine, volume, {created, rename});
    EXPECT_EQ(result.state, nexus::JournalReadState::ReconciliationRequired);
    EXPECT_FALSE(result.applied);
    EXPECT_TRUE(engine.SearchExact(L"would_be_added.txt").empty());
    std::filesystem::remove_all(root, ec);
}

TEST(NexusEngine, SnapshotRoundTripAndCorruptionAreAtomic) {
    const auto root = std::filesystem::temp_directory_path() / L"nexus_snapshot_test";
    const auto snapshot = std::filesystem::temp_directory_path() / L"nexus_snapshot_test.nxs";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::remove(snapshot, ec);
    std::filesystem::create_directory(root);
    std::ofstream(root / L"resume.txt") << "snapshot";

    nexus::Engine source;
    source.IndexPath(root);
    nexus::EngineJournalTestAccess::SetCheckpoint(source, L"C:\\", 19, 1234);
    ASSERT_TRUE(source.SaveSnapshot(snapshot));

    nexus::Engine restored;
    ASSERT_TRUE(restored.LoadSnapshot(snapshot));
    EXPECT_EQ(restored.SearchExact(L"resume.txt").size(), 1u);
    const auto checkpoint = restored.GetJournalCheckpoint(L"C:\\");
    ASSERT_TRUE(checkpoint.has_value());
    EXPECT_EQ(checkpoint->journal_id, 19u);
    EXPECT_EQ(checkpoint->next_usn, 1234);

    {
        std::fstream file(snapshot, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.good());
        file.seekg(-1, std::ios::end);
        char last{};
        file.read(&last, 1);
        last ^= 0x5A;
        file.seekp(-1, std::ios::end);
        file.write(&last, 1);
    }
    EXPECT_FALSE(restored.LoadSnapshot(snapshot));
    EXPECT_EQ(restored.SearchExact(L"resume.txt").size(), 1u);
    std::filesystem::remove(snapshot, ec);
    std::filesystem::remove_all(root, ec);
}
