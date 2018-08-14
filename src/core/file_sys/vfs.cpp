// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <numeric>
#include <string>
#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "core/file_sys/vfs.h"

namespace FileSys {

VfsFilesystem::VfsFilesystem(VirtualDir root_) : root(std::move(root_)) {}

VfsFilesystem::~VfsFilesystem() = default;

std::string VfsFilesystem::GetName() const {
    return root->GetName();
}

bool VfsFilesystem::IsReadable() const {
    return root->IsReadable();
}

bool VfsFilesystem::IsWritable() const {
    return root->IsWritable();
}

VfsEntryType VfsFilesystem::GetEntryType(std::string_view path_) const {
    const auto path = FileUtil::SanitizePath(path_);
    if (root->GetFileRelative(path) != nullptr)
        return VfsEntryType::File;
    if (root->GetDirectoryRelative(path) != nullptr)
        return VfsEntryType::Directory;

    return VfsEntryType::None;
}

VirtualFile VfsFilesystem::OpenFile(std::string_view path_, Mode perms) {
    const auto path = FileUtil::SanitizePath(path_);
    return root->GetFileRelative(path);
}

VirtualFile VfsFilesystem::CreateFile(std::string_view path_, Mode perms) {
    const auto path = FileUtil::SanitizePath(path_);
    return root->CreateFileRelative(path);
}

VirtualFile VfsFilesystem::CopyFile(std::string_view old_path_, std::string_view new_path_) {
    const auto old_path = FileUtil::SanitizePath(old_path_);
    const auto new_path = FileUtil::SanitizePath(new_path_);

    // VfsDirectory impls are only required to implement copy across the current directory.
    if (FileUtil::GetParentPath(old_path) == FileUtil::GetParentPath(new_path)) {
        if (!root->Copy(FileUtil::GetFilename(old_path), FileUtil::GetFilename(new_path)))
            return nullptr;
        return OpenFile(new_path, Mode::ReadWrite);
    }

    // Do it using RawCopy. Non-default impls are encouraged to optimize this.
    const auto old_file = OpenFile(old_path, Mode::Read);
    if (old_file == nullptr)
        return nullptr;
    auto new_file = OpenFile(new_path, Mode::Read);
    if (new_file != nullptr)
        return nullptr;
    new_file = CreateFile(new_path, Mode::Write);
    if (new_file == nullptr)
        return nullptr;
    if (!VfsRawCopy(old_file, new_file))
        return nullptr;
    return new_file;
}

VirtualFile VfsFilesystem::MoveFile(std::string_view old_path, std::string_view new_path) {
    const auto sanitized_old_path = FileUtil::SanitizePath(old_path);
    const auto sanitized_new_path = FileUtil::SanitizePath(new_path);

    // Again, non-default impls are highly encouraged to provide a more optimized version of this.
    auto out = CopyFile(sanitized_old_path, sanitized_new_path);
    if (out == nullptr)
        return nullptr;
    if (DeleteFile(sanitized_old_path))
        return out;
    return nullptr;
}

bool VfsFilesystem::DeleteFile(std::string_view path_) {
    const auto path = FileUtil::SanitizePath(path_);
    auto parent = OpenDirectory(FileUtil::GetParentPath(path), Mode::Write);
    if (parent == nullptr)
        return false;
    return parent->DeleteFile(FileUtil::GetFilename(path));
}

VirtualDir VfsFilesystem::OpenDirectory(std::string_view path_, Mode perms) {
    const auto path = FileUtil::SanitizePath(path_);
    return root->GetDirectoryRelative(path);
}

VirtualDir VfsFilesystem::CreateDirectory(std::string_view path_, Mode perms) {
    const auto path = FileUtil::SanitizePath(path_);
    return root->CreateDirectoryRelative(path);
}

VirtualDir VfsFilesystem::CopyDirectory(std::string_view old_path_, std::string_view new_path_) {
    const auto old_path = FileUtil::SanitizePath(old_path_);
    const auto new_path = FileUtil::SanitizePath(new_path_);

    // Non-default impls are highly encouraged to provide a more optimized version of this.
    auto old_dir = OpenDirectory(old_path, Mode::Read);
    if (old_dir == nullptr)
        return nullptr;
    auto new_dir = OpenDirectory(new_path, Mode::Read);
    if (new_dir != nullptr)
        return nullptr;
    new_dir = CreateDirectory(new_path, Mode::Write);
    if (new_dir == nullptr)
        return nullptr;

    for (const auto& file : old_dir->GetFiles()) {
        const auto x =
            CopyFile(old_path + DIR_SEP + file->GetName(), new_path + DIR_SEP + file->GetName());
        if (x == nullptr)
            return nullptr;
    }

    for (const auto& dir : old_dir->GetSubdirectories()) {
        const auto x =
            CopyDirectory(old_path + DIR_SEP + dir->GetName(), new_path + DIR_SEP + dir->GetName());
        if (x == nullptr)
            return nullptr;
    }

    return new_dir;
}

VirtualDir VfsFilesystem::MoveDirectory(std::string_view old_path, std::string_view new_path) {
    const auto sanitized_old_path = FileUtil::SanitizePath(old_path);
    const auto sanitized_new_path = FileUtil::SanitizePath(new_path);

    // Non-default impls are highly encouraged to provide a more optimized version of this.
    auto out = CopyDirectory(sanitized_old_path, sanitized_new_path);
    if (out == nullptr)
        return nullptr;
    if (DeleteDirectory(sanitized_old_path))
        return out;
    return nullptr;
}

bool VfsFilesystem::DeleteDirectory(std::string_view path_) {
    const auto path = FileUtil::SanitizePath(path_);
    auto parent = OpenDirectory(FileUtil::GetParentPath(path), Mode::Write);
    if (parent == nullptr)
        return false;
    return parent->DeleteSubdirectoryRecursive(FileUtil::GetFilename(path));
}

VfsFile::~VfsFile() = default;

std::string VfsFile::GetExtension() const {
    return std::string(FileUtil::GetExtensionFromFilename(GetName()));
}

VfsDirectory::~VfsDirectory() = default;

boost::optional<u8> VfsFile::ReadByte(size_t offset) const {
    u8 out{};
    size_t size = Read(&out, 1, offset);
    if (size == 1)
        return out;

    return boost::none;
}

std::vector<u8> VfsFile::ReadBytes(size_t size, size_t offset) const {
    std::vector<u8> out(size);
    size_t read_size = Read(out.data(), size, offset);
    out.resize(read_size);
    return out;
}

std::vector<u8> VfsFile::ReadAllBytes() const {
    return ReadBytes(GetSize());
}

bool VfsFile::WriteByte(u8 data, size_t offset) {
    return Write(&data, 1, offset) == 1;
}

size_t VfsFile::WriteBytes(const std::vector<u8>& data, size_t offset) {
    return Write(data.data(), data.size(), offset);
}

std::string VfsFile::GetFullPath() const {
    if (GetContainingDirectory() == nullptr)
        return "/" + GetName();

    return GetContainingDirectory()->GetFullPath() + "/" + GetName();
}

std::shared_ptr<VfsFile> VfsDirectory::GetFileRelative(std::string_view path) const {
    auto vec = FileUtil::SplitPathComponents(path);
    vec.erase(std::remove_if(vec.begin(), vec.end(), [](const auto& str) { return str.empty(); }),
              vec.end());
    if (vec.empty()) {
        return nullptr;
    }

    if (vec.size() == 1) {
        return GetFile(vec[0]);
    }

    auto dir = GetSubdirectory(vec[0]);
    for (size_t component = 1; component < vec.size() - 1; ++component) {
        if (dir == nullptr) {
            return nullptr;
        }

        dir = dir->GetSubdirectory(vec[component]);
    }

    if (dir == nullptr) {
        return nullptr;
    }

    return dir->GetFile(vec.back());
}

std::shared_ptr<VfsFile> VfsDirectory::GetFileAbsolute(std::string_view path) const {
    if (IsRoot()) {
        return GetFileRelative(path);
    }

    return GetParentDirectory()->GetFileAbsolute(path);
}

std::shared_ptr<VfsDirectory> VfsDirectory::GetDirectoryRelative(std::string_view path) const {
    auto vec = FileUtil::SplitPathComponents(path);
    vec.erase(std::remove_if(vec.begin(), vec.end(), [](const auto& str) { return str.empty(); }),
              vec.end());
    if (vec.empty()) {
        // TODO(DarkLordZach): Return this directory if path is '/' or similar. Can't currently
        // because of const-ness
        return nullptr;
    }

    auto dir = GetSubdirectory(vec[0]);
    for (size_t component = 1; component < vec.size(); ++component) {
        if (dir == nullptr) {
            return nullptr;
        }

        dir = dir->GetSubdirectory(vec[component]);
    }

    return dir;
}

std::shared_ptr<VfsDirectory> VfsDirectory::GetDirectoryAbsolute(std::string_view path) const {
    if (IsRoot()) {
        return GetDirectoryRelative(path);
    }

    return GetParentDirectory()->GetDirectoryAbsolute(path);
}

std::shared_ptr<VfsFile> VfsDirectory::GetFile(std::string_view name) const {
    const auto& files = GetFiles();
    const auto iter = std::find_if(files.begin(), files.end(),
                                   [&name](const auto& file1) { return name == file1->GetName(); });
    return iter == files.end() ? nullptr : *iter;
}

std::shared_ptr<VfsDirectory> VfsDirectory::GetSubdirectory(std::string_view name) const {
    const auto& subs = GetSubdirectories();
    const auto iter = std::find_if(subs.begin(), subs.end(),
                                   [&name](const auto& file1) { return name == file1->GetName(); });
    return iter == subs.end() ? nullptr : *iter;
}

bool VfsDirectory::IsRoot() const {
    return GetParentDirectory() == nullptr;
}

size_t VfsDirectory::GetSize() const {
    const auto& files = GetFiles();
    const auto sum_sizes = [](const auto& range) {
        return std::accumulate(range.begin(), range.end(), 0ULL,
                               [](const auto& f1, const auto& f2) { return f1 + f2->GetSize(); });
    };

    const auto file_total = sum_sizes(files);
    const auto& sub_dir = GetSubdirectories();
    const auto subdir_total = sum_sizes(sub_dir);

    return file_total + subdir_total;
}

std::shared_ptr<VfsFile> VfsDirectory::CreateFileRelative(std::string_view path) {
    auto vec = FileUtil::SplitPathComponents(path);
    vec.erase(std::remove_if(vec.begin(), vec.end(), [](const auto& str) { return str.empty(); }),
              vec.end());
    if (vec.empty()) {
        return nullptr;
    }

    if (vec.size() == 1) {
        return CreateFile(vec[0]);
    }

    auto dir = GetSubdirectory(vec[0]);
    if (dir == nullptr) {
        dir = CreateSubdirectory(vec[0]);
        if (dir == nullptr) {
            return nullptr;
        }
    }

    return dir->CreateFileRelative(FileUtil::GetPathWithoutTop(path));
}

std::shared_ptr<VfsFile> VfsDirectory::CreateFileAbsolute(std::string_view path) {
    if (IsRoot()) {
        return CreateFileRelative(path);
    }

    return GetParentDirectory()->CreateFileAbsolute(path);
}

std::shared_ptr<VfsDirectory> VfsDirectory::CreateDirectoryRelative(std::string_view path) {
    auto vec = FileUtil::SplitPathComponents(path);
    vec.erase(std::remove_if(vec.begin(), vec.end(), [](const auto& str) { return str.empty(); }),
              vec.end());
    if (vec.empty()) {
        return nullptr;
    }

    if (vec.size() == 1) {
        return CreateSubdirectory(vec[0]);
    }

    auto dir = GetSubdirectory(vec[0]);
    if (dir == nullptr) {
        dir = CreateSubdirectory(vec[0]);
        if (dir == nullptr) {
            return nullptr;
        }
    }

    return dir->CreateDirectoryRelative(FileUtil::GetPathWithoutTop(path));
}

std::shared_ptr<VfsDirectory> VfsDirectory::CreateDirectoryAbsolute(std::string_view path) {
    if (IsRoot()) {
        return CreateDirectoryRelative(path);
    }

    return GetParentDirectory()->CreateDirectoryAbsolute(path);
}

bool VfsDirectory::DeleteSubdirectoryRecursive(std::string_view name) {
    auto dir = GetSubdirectory(name);
    if (dir == nullptr) {
        return false;
    }

    bool success = true;
    for (const auto& file : dir->GetFiles()) {
        if (!DeleteFile(file->GetName())) {
            success = false;
        }
    }

    for (const auto& sdir : dir->GetSubdirectories()) {
        if (!dir->DeleteSubdirectoryRecursive(sdir->GetName())) {
            success = false;
        }
    }

    return success;
}

bool VfsDirectory::Copy(std::string_view src, std::string_view dest) {
    const auto f1 = GetFile(src);
    auto f2 = CreateFile(dest);
    if (f1 == nullptr || f2 == nullptr) {
        return false;
    }

    if (!f2->Resize(f1->GetSize())) {
        DeleteFile(dest);
        return false;
    }

    return f2->WriteBytes(f1->ReadAllBytes()) == f1->GetSize();
}

std::string VfsDirectory::GetFullPath() const {
    if (IsRoot())
        return GetName();

    return GetParentDirectory()->GetFullPath() + "/" + GetName();
}

bool ReadOnlyVfsDirectory::IsWritable() const {
    return false;
}

bool ReadOnlyVfsDirectory::IsReadable() const {
    return true;
}

std::shared_ptr<VfsDirectory> ReadOnlyVfsDirectory::CreateSubdirectory(std::string_view name) {
    return nullptr;
}

std::shared_ptr<VfsFile> ReadOnlyVfsDirectory::CreateFile(std::string_view name) {
    return nullptr;
}

bool ReadOnlyVfsDirectory::DeleteSubdirectory(std::string_view name) {
    return false;
}

bool ReadOnlyVfsDirectory::DeleteFile(std::string_view name) {
    return false;
}

bool ReadOnlyVfsDirectory::Rename(std::string_view name) {
    return false;
}

bool DeepEquals(const VirtualFile& file1, const VirtualFile& file2, size_t block_size) {
    if (file1->GetSize() != file2->GetSize())
        return false;

    std::vector<u8> f1_v(block_size);
    std::vector<u8> f2_v(block_size);
    for (size_t i = 0; i < file1->GetSize(); i += block_size) {
        auto f1_vs = file1->Read(f1_v.data(), block_size, i);
        auto f2_vs = file2->Read(f2_v.data(), block_size, i);

        if (f1_vs != f2_vs)
            return false;
        auto iters = std::mismatch(f1_v.begin(), f1_v.end(), f2_v.begin(), f2_v.end());
        if (iters.first != f1_v.end() && iters.second != f2_v.end())
            return false;
    }

    return true;
}

bool VfsRawCopy(VirtualFile src, VirtualFile dest) {
    if (src == nullptr || dest == nullptr)
        return false;
    if (!dest->Resize(src->GetSize()))
        return false;
    std::vector<u8> data = src->ReadAllBytes();
    return dest->WriteBytes(data, 0) == data.size();
}
} // namespace FileSys
