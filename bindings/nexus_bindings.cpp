#include "nexus/engine.h"
#include "nexus/usn_journal_reader.h"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace nexus;

PYBIND11_MODULE(_nexus, m) {
    m.doc() = "NEXUS Search native indexing and search engine";
    py::class_<FileRecord>(m, "FileRecord")
        .def_readonly("file_id", &FileRecord::file_id)
        .def_readonly("parent_id", &FileRecord::parent_id)
        .def_readonly("volume_id", &FileRecord::volume_id)
        .def_readonly("name", &FileRecord::name)
        .def_readonly("extension", &FileRecord::extension)
        .def_readonly("path", &FileRecord::path)
        .def_readonly("size_bytes", &FileRecord::size_bytes)
        .def_readonly("modified_time", &FileRecord::modified_time)
        .def_readonly("attributes", &FileRecord::attributes)
        .def_readonly("is_directory", &FileRecord::is_directory);
    py::class_<VolumeInfo>(m, "VolumeInfo")
        .def_readonly("root_path", &VolumeInfo::root_path)
        .def_readonly("filesystem", &VolumeInfo::filesystem)
        .def_readonly("label", &VolumeInfo::label)
        .def_readonly("drive_type", &VolumeInfo::drive_type)
        .def_readonly("ready", &VolumeInfo::ready);
    py::class_<IndexStats>(m, "IndexStats")
        .def_readonly("files", &IndexStats::files)
        .def_readonly("directories", &IndexStats::directories)
        .def_readonly("errors", &IndexStats::errors)
        .def_readonly("estimated_memory_bytes", &IndexStats::estimated_memory_bytes)
        .def_readonly("mft_fallbacks", &IndexStats::mft_fallbacks)
        .def_readonly("indexing", &IndexStats::indexing);
    py::class_<JournalCheckpoint>(m, "JournalCheckpoint")
        .def_readonly("journal_id", &JournalCheckpoint::journal_id)
        .def_readonly("next_usn", &JournalCheckpoint::next_usn);
    py::class_<SearchFilter>(m, "SearchFilter")
        .def(py::init<>())
        .def_readwrite("extension", &SearchFilter::extension)
        .def_readwrite("path_prefix", &SearchFilter::path_prefix)
        .def_readwrite("volume_id", &SearchFilter::volume_id)
        .def_readwrite("files_only", &SearchFilter::files_only)
        .def_readwrite("folders_only", &SearchFilter::folders_only)
        .def_readwrite("min_size_bytes", &SearchFilter::min_size_bytes)
        .def_readwrite("max_size_bytes", &SearchFilter::max_size_bytes)
        .def_readwrite("modified_after", &SearchFilter::modified_after)
        .def_readwrite("modified_before", &SearchFilter::modified_before);
    py::class_<UsnFileEntry>(m, "UsnFileEntry")
        .def_readonly("file_reference", &UsnFileEntry::file_reference)
        .def_readonly("parent_reference", &UsnFileEntry::parent_reference)
        .def_readonly("usn", &UsnFileEntry::usn)
        .def_readonly("timestamp_filetime", &UsnFileEntry::timestamp_filetime)
        .def_readonly("reason", &UsnFileEntry::reason)
        .def_readonly("attributes", &UsnFileEntry::attributes)
        .def_readonly("name", &UsnFileEntry::name);
    py::enum_<JournalReadState>(m, "JournalReadState")
        .value("Ok", JournalReadState::Ok)
        .value("ReconciliationRequired", JournalReadState::ReconciliationRequired)
        .value("Unsupported", JournalReadState::Unsupported)
        .value("AccessDenied", JournalReadState::AccessDenied)
        .value("Error", JournalReadState::Error);
    py::class_<JournalReadResult>(m, "JournalReadResult")
        .def_readonly("state", &JournalReadResult::state)
        .def_readonly("journal_id", &JournalReadResult::journal_id)
        .def_readonly("next_usn", &JournalReadResult::next_usn)
        .def_readonly("lowest_valid_usn", &JournalReadResult::lowest_valid_usn)
        .def_readonly("applied_changes", &JournalReadResult::applied_changes)
        .def_readonly("applied", &JournalReadResult::applied)
        .def_readonly("entries", &JournalReadResult::entries)
        .def_readonly("message", &JournalReadResult::message);
    m.def("read_usn_journal", &ReadUsnJournal, py::arg("volume_root"), py::arg("expected_journal_id"),
          py::arg("start_usn"), py::arg("buffer_bytes") = 1024 * 1024,
          py::call_guard<py::gil_scoped_release>());
    m.def("query_usn_journal_checkpoint", &QueryUsnJournalCheckpoint, py::arg("volume_root"),
          py::call_guard<py::gil_scoped_release>());
    py::class_<Engine>(m, "Engine")
        .def(py::init<>())
        .def("discover_volumes", &Engine::DiscoverVolumes)
        .def("index_path", [](Engine& self, const std::wstring& path) {
            py::gil_scoped_release release;
            self.IndexPath(std::filesystem::path(path));
        })
        .def("start_indexing", [](Engine& self, const std::vector<std::wstring>& roots) {
            std::vector<std::filesystem::path> paths;
            paths.reserve(roots.size());
            for (const auto& root : roots) paths.emplace_back(root);
            self.StartIndexing(paths);
        })
        .def("stop_indexing", &Engine::StopIndexing, py::call_guard<py::gil_scoped_release>())
        .def("sync_usn_journal", &Engine::SyncUsnJournal, py::arg("volume_root"), py::arg("expected_journal_id"),
             py::arg("start_usn"), py::call_guard<py::gil_scoped_release>())
        .def("set_journal_checkpoint", &Engine::SetJournalCheckpoint)
        .def("get_journal_checkpoint", &Engine::GetJournalCheckpoint)
        .def("save_snapshot", [](const Engine& self, const std::wstring& path) {
            py::gil_scoped_release release;
            return self.SaveSnapshot(std::filesystem::path(path));
        })
        .def("load_snapshot", [](Engine& self, const std::wstring& path) {
            py::gil_scoped_release release;
            return self.LoadSnapshot(std::filesystem::path(path));
        })
        .def("search_exact", &Engine::SearchExact, py::arg("query"), py::arg("limit") = 100,
             py::call_guard<py::gil_scoped_release>())
        .def("search_prefix", &Engine::SearchPrefix, py::arg("query"), py::arg("limit") = 100,
             py::call_guard<py::gil_scoped_release>())
        .def("search_substring", &Engine::SearchSubstring, py::arg("query"), py::arg("limit") = 100,
             py::call_guard<py::gil_scoped_release>())
        .def("search_filtered", &Engine::SearchFiltered, py::arg("query"), py::arg("mode"),
             py::arg("filter"), py::arg("limit") = 100,
             py::call_guard<py::gil_scoped_release>())
        .def("stats", &Engine::Stats)
        .def("clear", &Engine::Clear, py::call_guard<py::gil_scoped_release>());
}
