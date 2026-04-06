#pragma once
// =============================================================================
// dictionary.h
// ECE 1896 Senior Design - Team 13
// Author: Darren Ravichandra
// Modified: char* in PSRAM instead of std::string
// =============================================================================
#include <string>
#include <vector>
#include <map>
#include <stdexcept>

struct DictEntry {
    char* word;
    char* clue;
};

using WordDB = std::map<int, std::vector<DictEntry>>;

const std::vector<int> SUPPORTED_LENGTHS = {3, 4, 5};

// NOTE: load_dictionary() is no longer used — loading is done in the .ino
// These are kept for compatibility but can be removed
WordDB load_dictionary(const std::string& language, const std::string& dict_dir = "/dictionaries");
std::vector<std::string> list_available_languages(const std::string& dict_dir = "/dictionaries");
std::map<int, int> get_word_stats(const WordDB& word_db);