#include "manifest.hpp"
#include "yyjson_helper.hpp"
#include "log.hpp"
#include "app.hpp"

#include <yyjson.h>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace sphaira::manifest {

namespace {

void from_json(yyjson_val* json, Component& e) {
    // Direct yyjson parsing instead of macros to avoid potential issues
    if (auto val = yyjson_obj_get(json, "name")) {
        if (auto str = yyjson_get_str(val)) {
            e.name = str;
        }
    }
    if (auto val = yyjson_obj_get(json, "version")) {
        if (auto str = yyjson_get_str(val)) {
            e.version = str;
        }
    }
    if (auto val = yyjson_obj_get(json, "category")) {
        if (auto str = yyjson_get_str(val)) {
            e.category = str;
        }
    }
    if (auto val = yyjson_obj_get(json, "repo")) {
        if (auto str = yyjson_get_str(val)) {
            e.repo = str;
        }
    }
    // Parse files array
    if (auto val = yyjson_obj_get(json, "files")) {
        if (yyjson_is_arr(val)) {
            size_t idx, max;
            yyjson_val* item;
            yyjson_arr_foreach(val, idx, max, item) {
                if (yyjson_is_str(item)) {
                    if (auto str = yyjson_get_str(item)) {
                        e.files.emplace_back(str);
                    }
                }
            }
            log_write("[manifest] parsed component '%s' with %zu files\n", e.name.c_str(), e.files.size());
        } else {
            log_write("[manifest] component '%s': 'files' is not an array\n", e.name.c_str());
        }
    } else {
        log_write("[manifest] component '%s': no 'files' key found\n", e.name.c_str());
    }
}

void from_json(yyjson_val* json, std::map<std::string, Component>& components) {
    if (!yyjson_is_obj(json)) {
        return;
    }

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(json, &iter);
    yyjson_val* key;

    while ((key = yyjson_obj_iter_next(&iter))) {
        auto val = yyjson_obj_iter_get_val(key);
        if (!val || !yyjson_is_obj(val)) {
            continue;
        }

        const auto key_str = yyjson_get_str(key);
        if (!key_str) {
            continue;
        }

        Component comp;
        comp.id = key_str;
        from_json(val, comp);
        components[key_str] = comp;
    }
}

yyjson_mut_val* component_to_json(yyjson_mut_doc* doc, const Component& comp) {
    yyjson_mut_val* comp_obj = yyjson_mut_obj(doc);

    yyjson_mut_obj_add_str(doc, comp_obj, "name", comp.name.c_str());
    yyjson_mut_obj_add_str(doc, comp_obj, "version", comp.version.c_str());
    yyjson_mut_obj_add_str(doc, comp_obj, "category", comp.category.c_str());
    yyjson_mut_obj_add_str(doc, comp_obj, "repo", comp.repo.c_str());

    yyjson_mut_val* files_arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, comp_obj, "files", files_arr);

    for (const auto& file : comp.files) {
        yyjson_mut_arr_add_str(doc, files_arr, file.c_str());
    }

    return comp_obj;
}

fs::FsPath normalize_path(const std::string& file) {
    fs::FsPath path;
    if (!file.empty() && file[0] != '/') {
        std::snprintf(path, sizeof(path), "/%s", file.c_str());
    } else {
        std::snprintf(path, sizeof(path), "%s", file.c_str());
    }
    return path;
}

fs::FsPath disabled_path_for(const std::string& id, const std::string& file) {
    auto source_path = normalize_path(file);
    const char* relative = source_path.s;
    while (*relative == '/') {
        relative++;
    }

    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%s/%s", DISABLED_COMPONENTS_DIR, id.c_str(), relative);
    return path;
}

fs::FsPath disabled_root_for(const std::string& id) {
    fs::FsPath path;
    std::snprintf(path, sizeof(path), "%s/%s", DISABLED_COMPONENTS_DIR, id.c_str());
    return path;
}

bool is_shared_file(const Manifest& m, const std::string& owner_id, const std::string& file) {
    const auto path = normalize_path(file).toString();
    for (const auto& [id, comp] : m.components) {
        if (id == owner_id) {
            continue;
        }

        for (const auto& other_file : comp.files) {
            if (fs::FsPath::path_equal(path, normalize_path(other_file).toString())) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool Component::isProtected() const {
    return isProtectedComponent(id);
}

bool load(Manifest& out) {
    auto doc = yyjson_read_file(MANIFEST_PATH, YYJSON_READ_NOFLAG, nullptr, nullptr);
    if (!doc) {
        log_write("manifest: failed to read %s\n", MANIFEST_PATH);
        return false;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        log_write("manifest: invalid root object\n");
        return false;
    }

    // Parse top-level fields
    if (auto val = yyjson_obj_get(root, "pack_name")) {
        if (yyjson_is_str(val)) {
            out.pack_name = yyjson_get_str(val);
        }
    }

    if (auto val = yyjson_obj_get(root, "build_date")) {
        if (yyjson_is_str(val)) {
            out.build_date = yyjson_get_str(val);
        }
    }

    if (auto val = yyjson_obj_get(root, "builder_version")) {
        if (yyjson_is_str(val)) {
            out.builder_version = yyjson_get_str(val);
        }
    }

    if (auto val = yyjson_obj_get(root, "supported_firmware")) {
        if (yyjson_is_str(val)) {
            out.supported_firmware = yyjson_get_str(val);
        }
    }

    if (auto val = yyjson_obj_get(root, "content_hash")) {
        if (yyjson_is_str(val)) {
            out.content_hash = yyjson_get_str(val);
        }
    }

    // Parse components
    if (auto components_val = yyjson_obj_get(root, "components")) {
        from_json(components_val, out.components);
    }

    log_write("manifest: loaded %zu components from %s\n", out.components.size(), MANIFEST_PATH);
    return true;
}

bool save(const Manifest& m) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        return false;
    }
    ON_SCOPE_EXIT(yyjson_mut_doc_free(doc));

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    // Add top-level fields
    yyjson_mut_obj_add_str(doc, root, "pack_name", m.pack_name.c_str());
    yyjson_mut_obj_add_str(doc, root, "build_date", m.build_date.c_str());
    yyjson_mut_obj_add_str(doc, root, "builder_version", m.builder_version.c_str());
    yyjson_mut_obj_add_str(doc, root, "supported_firmware", m.supported_firmware.c_str());
    yyjson_mut_obj_add_str(doc, root, "content_hash", m.content_hash.c_str());

    // Add components
    yyjson_mut_val* components_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "components", components_obj);

    for (const auto& [id, comp] : m.components) {
        yyjson_mut_obj_add_val(doc, components_obj, id.c_str(), component_to_json(doc, comp));
    }

    // Write to file
    yyjson_write_err err;
    bool success = yyjson_mut_write_file(MANIFEST_PATH, doc, YYJSON_WRITE_PRETTY, nullptr, &err);

    if (!success) {
        log_write("manifest: failed to write %s: %s\n", MANIFEST_PATH, err.msg);
    } else {
        log_write("manifest: saved %zu components to %s\n", m.components.size(), MANIFEST_PATH);
    }

    return success;
}

bool exists() {
    FILE* f = fopen(MANIFEST_PATH, "r");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

bool loadDisabled(DisabledComponents& out) {
    out.components.clear();

    auto doc = yyjson_read_file(DISABLED_COMPONENTS_PATH, YYJSON_READ_NOFLAG, nullptr, nullptr);
    if (!doc) {
        log_write("manifest: no disabled components metadata at %s\n", DISABLED_COMPONENTS_PATH);
        return true;
    }
    ON_SCOPE_EXIT(yyjson_doc_free(doc));

    auto root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        log_write("manifest: invalid disabled components root object\n");
        return false;
    }

    if (auto components_val = yyjson_obj_get(root, "components")) {
        from_json(components_val, out.components);
    }

    log_write("manifest: loaded %zu disabled components from %s\n",
              out.components.size(), DISABLED_COMPONENTS_PATH);
    return true;
}

bool saveDisabled(const DisabledComponents& disabled) {
    fs::FsNativeSd fs;
    if (R_FAILED(fs.GetFsOpenResult())) {
        return false;
    }

    fs.CreateDirectoryRecursivelyWithPath(DISABLED_COMPONENTS_PATH);

    yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
    if (!doc) {
        return false;
    }
    ON_SCOPE_EXIT(yyjson_mut_doc_free(doc));

    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);

    yyjson_mut_val* components_obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_val(doc, root, "components", components_obj);

    for (const auto& [id, comp] : disabled.components) {
        yyjson_mut_obj_add_val(doc, components_obj, id.c_str(), component_to_json(doc, comp));
    }

    yyjson_write_err err;
    bool success = yyjson_mut_write_file(DISABLED_COMPONENTS_PATH, doc, YYJSON_WRITE_PRETTY, nullptr, &err);

    if (!success) {
        log_write("manifest: failed to write %s: %s\n", DISABLED_COMPONENTS_PATH, err.msg);
    } else {
        log_write("manifest: saved %zu disabled components to %s\n",
                  disabled.components.size(), DISABLED_COMPONENTS_PATH);
    }

    return success;
}

std::vector<Component> getComponents(const Manifest& m) {
    std::vector<Component> result;
    result.reserve(m.components.size());

    for (const auto& [id, comp] : m.components) {
        result.push_back(comp);
    }

    // Sort by category, then by name
    std::sort(result.begin(), result.end(), [](const Component& a, const Component& b) {
        if (a.category != b.category) {
            return a.category < b.category;
        }
        return a.name < b.name;
    });

    return result;
}

std::vector<Component> getUninstallableComponents(const Manifest& m) {
    std::vector<Component> result;
    result.reserve(m.components.size());

    for (const auto& [id, comp] : m.components) {
        if (!isProtectedComponent(id)) {
            result.push_back(comp);
        }
    }

    // Sort by category, then by name
    std::sort(result.begin(), result.end(), [](const Component& a, const Component& b) {
        if (a.category != b.category) {
            return a.category < b.category;
        }
        return a.name < b.name;
    });

    return result;
}

bool removeComponent(Manifest& m, const std::string& id, fs::Fs* fs) {
    if (isProtectedComponent(id)) {
        log_write("[UNINSTALL] cannot remove protected component: %s\n", id.c_str());
        return false;
    }

    auto it = m.components.find(id);
    if (it == m.components.end()) {
        log_write("[UNINSTALL] component not found: %s\n", id.c_str());
        return false;
    }

    const Component& comp = it->second;
    log_write("[UNINSTALL] removing component %s (%s) version %s with %zu files\n",
              id.c_str(), comp.name.c_str(), comp.version.c_str(), comp.files.size());

    // Delete all files associated with this component
    int deleted_count = 0;
    int failed_count = 0;
    for (const auto& file : comp.files) {
        fs::FsPath path;
        if (file[0] != '/') {
            std::snprintf(path, sizeof(path), "/%s", file.c_str());
        } else {
            std::snprintf(path, sizeof(path), "%s", file.c_str());
        }

        log_write("[UNINSTALL] attempting to delete %s\n", path.s);

        // Check if file/directory exists first
        bool is_file = fs->FileExists(path);
        bool is_dir = fs->DirExists(path);

        if (!is_file && !is_dir) {
            log_write("[UNINSTALL] %s does not exist, skipping\n", path.s);
            failed_count++;
            continue;
        }

        Result rc;
        if (is_file) {
            rc = fs->DeleteFile(path);
            log_write("[UNINSTALL] DeleteFile(%s) = 0x%X\n", path.s, rc);
        } else {
            rc = fs->DeleteDirectoryRecursively(path);
            log_write("[UNINSTALL] DeleteDirectoryRecursively(%s) = 0x%X\n", path.s, rc);
        }

        if (R_SUCCEEDED(rc)) {
            log_write("[UNINSTALL] successfully deleted %s\n", path.s);
            deleted_count++;
        } else {
            log_write("[UNINSTALL] failed to delete %s (error: 0x%X)\n", path.s, rc);
            failed_count++;
        }
    }

    log_write("[UNINSTALL] component %s deletion summary: %d deleted, %d failed\n",
              id.c_str(), deleted_count, failed_count);

    // Remove from manifest regardless of individual file deletions
    m.components.erase(it);
    log_write("[UNINSTALL] removed component %s from manifest\n", id.c_str());

    return true;  // Return true even if some files failed to delete
}

bool disableComponent(Manifest& m, DisabledComponents& disabled, const std::string& id, fs::Fs* fs) {
    if (isProtectedComponent(id)) {
        log_write("[DISABLE] cannot disable protected component: %s\n", id.c_str());
        return false;
    }

    auto it = m.components.find(id);
    if (it == m.components.end()) {
        log_write("[DISABLE] component not found: %s\n", id.c_str());
        return false;
    }

    if (disabled.components.count(id)) {
        log_write("[DISABLE] component already disabled: %s\n", id.c_str());
        return false;
    }

    const Component comp = it->second;
    log_write("[DISABLE] disabling component %s (%s) with %zu files\n",
              id.c_str(), comp.name.c_str(), comp.files.size());

    Result rc = fs->CreateDirectoryRecursively(DISABLED_COMPONENTS_DIR);
    if (R_FAILED(rc)) {
        log_write("[DISABLE] failed to create disabled components dir (error: 0x%X)\n", rc);
        return false;
    }

    int moved_count = 0;
    int skipped_count = 0;
    int failed_count = 0;
    std::vector<std::pair<fs::FsPath, fs::FsPath>> moved_paths;

    for (const auto& file : comp.files) {
        const auto source = normalize_path(file);
        const auto destination = disabled_path_for(id, file);

        if (is_shared_file(m, id, file)) {
            log_write("[DISABLE] skipping shared file %s\n", source.s);
            skipped_count++;
            continue;
        }

        const bool is_file = fs->FileExists(source);
        const bool is_dir = fs->DirExists(source);

        if (!is_file && !is_dir) {
            log_write("[DISABLE] %s does not exist, skipping\n", source.s);
            skipped_count++;
            continue;
        }

        if (fs->FileExists(destination) || fs->DirExists(destination)) {
            log_write("[DISABLE] destination already exists, refusing to overwrite: %s\n", destination.s);
            failed_count++;
            continue;
        }

        rc = fs->CreateDirectoryRecursivelyWithPath(destination);
        if (R_FAILED(rc)) {
            log_write("[DISABLE] failed to create destination path %s (error: 0x%X)\n", destination.s, rc);
            failed_count++;
            continue;
        }

        if (is_file) {
            rc = fs->RenameFile(source, destination);
        } else {
            rc = fs->RenameDirectory(source, destination);
        }

        if (R_SUCCEEDED(rc)) {
            log_write("[DISABLE] moved %s -> %s\n", source.s, destination.s);
            moved_paths.emplace_back(source, destination);
            moved_count++;
        } else {
            log_write("[DISABLE] failed to move %s -> %s (error: 0x%X)\n", source.s, destination.s, rc);
            failed_count++;
        }
    }

    log_write("[DISABLE] component %s summary: %d moved, %d skipped, %d failed\n",
              id.c_str(), moved_count, skipped_count, failed_count);

    if (failed_count > 0) {
        for (auto it = moved_paths.rbegin(); it != moved_paths.rend(); ++it) {
            const auto& source = it->first;
            const auto& destination = it->second;
            Result rc;
            if (fs->FileExists(destination)) {
                rc = fs->RenameFile(destination, source);
            } else if (fs->DirExists(destination)) {
                rc = fs->RenameDirectory(destination, source);
            } else {
                continue;
            }

            log_write("[DISABLE] rollback %s -> %s = 0x%X\n", destination.s, source.s, rc);
        }
        log_write("[DISABLE] refusing to disable %s due to failed moves\n", id.c_str());
        return false;
    }

    disabled.components[id] = comp;
    m.components.erase(it);
    return true;
}

bool enableComponent(Manifest& m, DisabledComponents& disabled, const std::string& id, fs::Fs* fs) {
    auto it = disabled.components.find(id);
    if (it == disabled.components.end()) {
        log_write("[ENABLE] disabled component not found: %s\n", id.c_str());
        return false;
    }

    if (m.components.count(id)) {
        log_write("[ENABLE] active component already exists: %s\n", id.c_str());
        return false;
    }

    const Component comp = it->second;
    int moved_count = 0;
    int skipped_count = 0;
    int failed_count = 0;
    std::vector<std::pair<fs::FsPath, fs::FsPath>> moved_paths;

    for (const auto& file : comp.files) {
        const auto source = disabled_path_for(id, file);
        const auto destination = normalize_path(file);

        const bool is_file = fs->FileExists(source);
        const bool is_dir = fs->DirExists(source);

        if (!is_file && !is_dir) {
            log_write("[ENABLE] disabled file %s does not exist, skipping\n", source.s);
            skipped_count++;
            continue;
        }

        if (fs->FileExists(destination) || fs->DirExists(destination)) {
            log_write("[ENABLE] live destination exists, refusing to overwrite: %s\n", destination.s);
            failed_count++;
            continue;
        }

        Result rc = fs->CreateDirectoryRecursivelyWithPath(destination);
        if (R_FAILED(rc)) {
            log_write("[ENABLE] failed to create destination path %s (error: 0x%X)\n", destination.s, rc);
            failed_count++;
            continue;
        }

        if (is_file) {
            rc = fs->RenameFile(source, destination);
        } else {
            rc = fs->RenameDirectory(source, destination);
        }

        if (R_SUCCEEDED(rc)) {
            log_write("[ENABLE] moved %s -> %s\n", source.s, destination.s);
            moved_paths.emplace_back(source, destination);
            moved_count++;
        } else {
            log_write("[ENABLE] failed to move %s -> %s (error: 0x%X)\n", source.s, destination.s, rc);
            failed_count++;
        }
    }

    log_write("[ENABLE] component %s summary: %d moved, %d skipped, %d failed\n",
              id.c_str(), moved_count, skipped_count, failed_count);

    if (failed_count > 0) {
        for (auto it = moved_paths.rbegin(); it != moved_paths.rend(); ++it) {
            const auto& source = it->first;
            const auto& destination = it->second;
            Result rc;
            if (fs->FileExists(destination)) {
                rc = fs->RenameFile(destination, source);
            } else if (fs->DirExists(destination)) {
                rc = fs->RenameDirectory(destination, source);
            } else {
                continue;
            }

            log_write("[ENABLE] rollback %s -> %s = 0x%X\n", destination.s, source.s, rc);
        }
        log_write("[ENABLE] refusing to enable %s due to failed moves\n", id.c_str());
        return false;
    }

    m.components[id] = comp;
    disabled.components.erase(it);
    return true;
}

bool deleteDisabledComponent(DisabledComponents& disabled, const std::string& id, fs::Fs* fs) {
    auto it = disabled.components.find(id);
    if (it == disabled.components.end()) {
        log_write("[DELETE DISABLED] disabled component not found: %s\n", id.c_str());
        return false;
    }

    const auto root = disabled_root_for(id);
    if (fs->DirExists(root)) {
        Result rc = fs->DeleteDirectoryRecursively(root);
        if (R_FAILED(rc)) {
            log_write("[DELETE DISABLED] failed to delete %s (error: 0x%X)\n", root.s, rc);
            return false;
        }
    }

    disabled.components.erase(it);
    log_write("[DELETE DISABLED] permanently removed disabled component %s\n", id.c_str());
    return true;
}

int removeComponents(Manifest& m, const std::vector<std::string>& ids, fs::Fs* fs) {
    log_write("[UNINSTALL] batch removing %zu components\n", ids.size());
    int count = 0;
    for (const auto& id : ids) {
        if (removeComponent(m, id, fs)) {
            count++;
        }
    }
    log_write("[UNINSTALL] batch removal complete: %d/%zu removed\n", count, ids.size());
    return count;
}

bool isProtectedComponent(const std::string& id) {
    // God Mode bypasses all component protections
    if (sphaira::App::GetGodModeEnabled()) {
        return false;
    }

    for (const char* protected_id : PROTECTED_COMPONENTS) {
        if (id == protected_id) {
            return true;
        }
    }
    return false;
}

} // namespace sphaira::manifest
