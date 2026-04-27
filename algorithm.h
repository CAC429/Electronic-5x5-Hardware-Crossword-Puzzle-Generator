// =============================================================================
// algorithm.h
// ECE 1896 Senior Design - Team 13
// Author: Darren Ravichandra
// Modified for ESP32 — char* DictEntry, WordIndex stores int indices
// =============================================================================

#pragma once
#include "dictionary.h"

#include <string>
#include <vector>
#include <set>
#include <map>
#include <optional>

// SECTION 1 — DATA STRUCTURES
const int  GRID_SIZE = 5;
const char BLACK_SQ  = '#';
const char EMPTY_SQ  = '\0';

using Cell    = std::pair<int, int>;
using Pattern = std::set<Cell>;

struct WordSlot {
    std::string direction;
    int         start_row;
    int         start_col;
    int         length;
    std::string answer;
    std::string clue;

    std::vector<Cell> cells() const;
};

struct PuzzleGrid {
    std::string language;
    char        grid[GRID_SIZE][GRID_SIZE];
    std::vector<WordSlot> slots;

    PuzzleGrid(const Pattern& pattern, const std::string& language = "english");

    char get_cell(int row, int col) const { return grid[row][col]; }
    void set_cell(int row, int col, char value) { grid[row][col] = value; }
};

// SECTION 2 — BLACK SQUARE PATTERNS
extern const std::vector<Pattern> PATTERNS;
bool is_valid_pattern(const Pattern& pattern);

// SECTION 3 — CSP + BACKTRACKING
using WordIndex = std::map<int, std::vector<std::map<char, std::vector<int>>>>;

WordIndex build_index(const WordDB& word_db);

std::vector<int> get_candidate_indices(
    const WordSlot& slot,
    const char grid[GRID_SIZE][GRID_SIZE],
    const WordDB& word_db,
    const std::set<std::string>& used_words,
    const WordIndex* index = nullptr
);

bool backtrack(
    std::vector<WordSlot>& slots,
    char grid[GRID_SIZE][GRID_SIZE],
    const WordDB& word_db,
    std::set<std::string>& used_words,
    const WordIndex* word_index = nullptr
);

std::optional<PuzzleGrid> generate_puzzle(
    const WordDB& word_db,
    const std::string& language = "english",
    int max_attempts = 20
);

// SECTION 4 — CLUE MANAGEMENT
const WordSlot* get_active_clue(
    const PuzzleGrid& puzzle,
    int selected_row,
    int selected_col,
    const std::string& direction = "across"
);

// SECTION 5 — DISPLAY / OUTPUT
void print_grid(const PuzzleGrid& puzzle);
void print_clues(const PuzzleGrid& puzzle);
void print_puzzle(const PuzzleGrid& puzzle);