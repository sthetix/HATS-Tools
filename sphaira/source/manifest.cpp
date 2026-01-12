#include "manifest.hpp"
#include "yyjson_helper.hpp"
#include "log.hpp"
#include "app.hpp"

#include <yyjson.h>
#include <cstring>
#include <algorithm>

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
        yyjson_mut_val* comp_obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, components_obj, id.c_str(), comp_obj);

        yyjson_mut_obj_add_str(doc, comp_obj, "name", comp.name.c_str());
        yyjson_mut_obj_add_str(doc, comp_obj, "version", comp.version.c_str());
        yyjson_mut_obj_add_str(doc, comp_obj, "category", comp.category.c_str());
        yyjson_mut_obj_add_str(doc, comp_obj, "repo", comp.repo.c_str());

        yyjson_mut_val* files_arr = yyjson_mut_arr(doc);
        yyjson_mut_obj_add_val(doc, comp_obj, "files", files_arr);

        for (const auto& file : comp.files) {
            yyjson_mut_arr_add_str(doc, files_arr, file.c_str());
        }
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
