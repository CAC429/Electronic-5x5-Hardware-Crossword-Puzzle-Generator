// =============================================================================
// dictionary.cpp
// ECE 1896 Senior Design - Team 13
// Author: Darren Ravichandra
// Modified for ESP32 LittleFS + PSRAM — char* DictEntry
// =============================================================================

#include "dictionary.h"
#include <LittleFS.h>
#include <Arduino.h>
#include <algorithm>
#include <set>

// =============================================================================
// load_dictionary — NOT USED (loading is done in .ino via ps_malloc)
// Kept as a stub so the linker doesn't complain
// =============================================================================
WordDB load_dictionary(const std::string& language, const std::string& dict_dir) {
    WordDB word_db;
    Serial.println("[WARN] load_dictionary() stub called — use loadDictionary() in .ino instead");
    return word_db;
}

// =============================================================================
// list_available_languages
// =============================================================================
std::vector<std::string> list_available_languages(const std::string& dict_dir) {
    std::vector<std::string> complete;
    std::set<std::string> candidates;

    File root = LittleFS.open(dict_dir.c_str(), "r");
    if (!root || !root.isDirectory()) return complete;

    File entry = root.openNextFile();
    while (entry) {
        std::string fname = std::string(entry.name());
        size_t slash = fname.rfind('/');
        if (slash != std::string::npos) fname = fname.substr(slash + 1);

        if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".txt") {
            std::string base = fname.substr(0, fname.size() - 4);
            size_t underscore = base.rfind('_');
            if (underscore != std::string::npos) {
                std::string lang       = base.substr(0, underscore);
                std::string length_str = base.substr(underscore + 1);
                bool is_num = !length_str.empty() &&
                              std::all_of(length_str.begin(), length_str.end(), ::isdigit);
                if (is_num) candidates.insert(lang);
            }
        }
        entry = root.openNextFile();
    }

    for (const auto& lang : candidates) {
        bool has_all = true;
        for (int l : SUPPORTED_LENGTHS) {
            std::string path = dict_dir + "/" + lang + "_" + std::to_string(l) + ".txt";
            if (!LittleFS.exists(path.c_str())) { has_all = false; break; }
        }
        if (has_all) complete.push_back(lang);
    }

    std::sort(complete.begin(), complete.end());
    return complete;
}

// =============================================================================
// get_word_stats
// =============================================================================
std::map<int, int> get_word_stats(const WordDB& word_db) {
    std::map<int, int> stats;
    for (const auto& [length, words] : word_db)
        stats[length] = static_cast<int>(words.size());
    return stats;
}