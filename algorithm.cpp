// =============================================================================
// algorithm.cpp
// ECE 1896 Senior Design - Team 13
// Author: Darren Ravichandra
// Modified for ESP32 — char* DictEntry, PSRAM-friendly
// =============================================================================

#include "algorithm.h"
#include <Arduino.h>
#include <algorithm>
#include <random>
#include <cassert>
#include <cstring>

static std::mt19937 rng(esp_random());

// =============================================================================
// SECTION 1 — DATA STRUCTURES
// =============================================================================

std::vector<Cell> WordSlot::cells() const {
    std::vector<Cell> result;
    for (int i = 0; i < length; i++) {
        if (direction == "across")
            result.push_back({start_row, start_col + i});
        else
            result.push_back({start_row + i, start_col});
    }
    return result;
}

PuzzleGrid::PuzzleGrid(const Pattern& pattern, const std::string& lang)
    : language(lang)
{
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            grid[r][c] = EMPTY_SQ;

    for (const auto& [r, c] : pattern)
        grid[r][c] = BLACK_SQ;

    for (int r = 0; r < GRID_SIZE; r++) {
        int c = 0;
        while (c < GRID_SIZE) {
            if (grid[r][c] != BLACK_SQ) {
                int start_c = c;
                while (c < GRID_SIZE && grid[r][c] != BLACK_SQ) c++;
                int length = c - start_c;
                if (length >= 3)
                    slots.push_back({"across", r, start_c, length, "", ""});
            } else {
                c++;
            }
        }
    }

    for (int col = 0; col < GRID_SIZE; col++) {
        int r = 0;
        while (r < GRID_SIZE) {
            if (grid[r][col] != BLACK_SQ) {
                int start_r = r;
                while (r < GRID_SIZE && grid[r][col] != BLACK_SQ) r++;
                int length = r - start_r;
                if (length >= 3)
                    slots.push_back({"down", start_r, col, length, "", ""});
            } else {
                r++;
            }
        }
    }
}

// =============================================================================
// SECTION 2 — BLACK SQUARE PATTERNS
// =============================================================================

const std::vector<Pattern> PATTERNS = {
    { {0,0},{0,1},{1,0},{3,4},{4,3},{4,4} },
    { {0,3},{0,4},{1,4},{3,0},{4,0},{4,1} },
    { {0,0},{4,4} },
    { {0,4},{4,0} },
    { {0,0},{0,1},{4,3},{4,4} },
    { {0,3},{0,4},{4,0},{4,1} },
};

bool is_valid_pattern(const Pattern& pattern) {
    char grid[GRID_SIZE][GRID_SIZE];
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            grid[r][c] = (pattern.count({r, c}) ? BLACK_SQ : EMPTY_SQ);

    std::vector<Cell> white_cells;
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            if (grid[r][c] != BLACK_SQ)
                white_cells.push_back({r, c});

    if (white_cells.empty()) return false;

    std::set<Cell> visited;
    std::vector<Cell> stack = {white_cells[0]};
    const int dr[] = {-1, 1, 0, 0};
    const int dc[] = {0, 0, -1, 1};

    while (!stack.empty()) {
        auto [r, c] = stack.back(); stack.pop_back();
        if (visited.count({r, c})) continue;
        visited.insert({r, c});
        for (int d = 0; d < 4; d++) {
            int nr = r + dr[d], nc = c + dc[d];
            if (nr >= 0 && nr < GRID_SIZE && nc >= 0 && nc < GRID_SIZE
                && grid[nr][nc] != BLACK_SQ && !visited.count({nr, nc}))
                stack.push_back({nr, nc});
        }
    }

    if (visited.size() != white_cells.size()) return false;

    PuzzleGrid temp(pattern);
    int across_count = 0, down_count = 0;
    for (const auto& s : temp.slots) {
        if (s.direction == "across") across_count++;
        else down_count++;
    }
    if (across_count < 2 || down_count < 2) return false;

    std::set<Cell> covered;
    for (const auto& slot : temp.slots)
        for (const auto& cell : slot.cells())
            covered.insert(cell);
    if (covered != std::set<Cell>(white_cells.begin(), white_cells.end()))
        return false;

    for (int r = 0; r < GRID_SIZE; r++) {
        int run = 0;
        for (int c = 0; c < GRID_SIZE; c++) {
            if (grid[r][c] != BLACK_SQ) run++;
            else { if (run > 0 && run < 3) return false; run = 0; }
        }
        if (run > 0 && run < 3) return false;
    }
    for (int c = 0; c < GRID_SIZE; c++) {
        int run = 0;
        for (int r = 0; r < GRID_SIZE; r++) {
            if (grid[r][c] != BLACK_SQ) run++;
            else { if (run > 0 && run < 3) return false; run = 0; }
        }
        if (run > 0 && run < 3) return false;
    }

    return true;
}

// =============================================================================
// SECTION 3 — CONSTRAINT SATISFACTION + BACKTRACKING
// =============================================================================

WordIndex build_index(const WordDB& word_db) {
    WordIndex index;
    for (const auto& [length, entries] : word_db) {
        index[length].resize(length);
        for (int i = 0; i < (int)entries.size(); i++) {
            for (int pos = 0; pos < length; pos++) {
                char letter = entries[i].word[pos];  // char* indexing works fine
                index[length][pos][letter].push_back(i);
            }
        }
    }
    return index;
}

// Returns indices into word_db[slot.length], not DictEntry copies
std::vector<int> get_candidate_indices(
    const WordSlot& slot,
    const char grid[GRID_SIZE][GRID_SIZE],
    const WordDB& word_db,
    const std::set<std::string>& used_words,
    const WordIndex* index)
{
    int length = slot.length;
    auto slot_cells = slot.cells();

    std::map<int, char> constraints;
    for (int i = 0; i < (int)slot_cells.size(); i++) {
        auto [r, c] = slot_cells[i];
        char existing = grid[r][c];
        if (existing != EMPTY_SQ && existing != BLACK_SQ)
            constraints[i] = existing;
    }

    const auto& entries = word_db.at(length);
    std::vector<int> candidate_indices;

    if (index && index->count(length)) {
        const auto& len_index = index->at(length);

        if (!constraints.empty()) {
            int best_pos = -1;
            size_t best_count = SIZE_MAX;
            for (const auto& [pos, letter] : constraints) {
                auto it = len_index[pos].find(letter);
                size_t count = (it != len_index[pos].end()) ? it->second.size() : 0;
                if (count < best_count) {
                    best_count = count;
                    best_pos = pos;
                }
            }

            if (best_pos >= 0) {
                char best_letter = constraints.at(best_pos);
                auto it = len_index[best_pos].find(best_letter);
                if (it != len_index[best_pos].end())
                    candidate_indices = it->second;

                for (const auto& [pos, letter] : constraints) {
                    if (pos == best_pos) continue;
                    candidate_indices.erase(
                        std::remove_if(candidate_indices.begin(), candidate_indices.end(),
                            [&](int idx) { return entries[idx].word[pos] != letter; }),
                        candidate_indices.end()
                    );
                }
            }
        } else {
            candidate_indices.resize(entries.size());
            for (int i = 0; i < (int)entries.size(); i++) candidate_indices[i] = i;
        }
    } else {
        for (int i = 0; i < (int)entries.size(); i++) {
            bool fits = true;
            for (const auto& [pos, letter] : constraints) {
                if (entries[i].word[pos] != letter) { fits = false; break; }
            }
            if (fits) candidate_indices.push_back(i);
        }
    }

    // Filter used words — compare char* as std::string for set lookup
    std::vector<int> filtered;
    filtered.reserve(candidate_indices.size());
    for (int idx : candidate_indices) {
        std::string w(entries[idx].word);  // temporary string for set lookup
        if (!used_words.count(w))
            filtered.push_back(idx);
    }

    std::shuffle(filtered.begin(), filtered.end(), rng);
    return filtered;
}

bool backtrack(
    std::vector<WordSlot>& slots,
    char grid[GRID_SIZE][GRID_SIZE],
    const WordDB& word_db,
    std::set<std::string>& used_words,
    const WordIndex* word_index)
{
    yield();

    std::vector<WordSlot*> remaining;
    for (auto& slot : slots)
        if (slot.answer.empty()) remaining.push_back(&slot);

    if (remaining.empty()) return true;

    WordSlot* best_slot = nullptr;
    size_t best_count = SIZE_MAX;

    for (auto* slot : remaining) {
        auto cands = get_candidate_indices(*slot, grid, word_db, used_words, word_index);
        size_t n = cands.size();
        if (n == 0) return false;
        bool better = (n < best_count) ||
                      (n == best_count && best_slot && slot->length > best_slot->length);
        if (better) { best_count = n; best_slot = slot; }
    }

    if (!best_slot) return false;

    auto candidate_indices = get_candidate_indices(*best_slot, grid, word_db, used_words, word_index);
    if (candidate_indices.empty()) return false;

    const auto& entries = word_db.at(best_slot->length);
    auto slot_cells = best_slot->cells();

    std::vector<char> saved;
    for (const auto& [r, c] : slot_cells)
        saved.push_back(grid[r][c]);

    for (int idx : candidate_indices) {
        const DictEntry& entry = entries[idx];
        std::string word_str(entry.word);  // temp string for answer/used_words

        for (int i = 0; i < (int)slot_cells.size(); i++) {
            auto [r, c] = slot_cells[i];
            grid[r][c] = entry.word[i];
        }
        best_slot->answer = word_str;
        best_slot->clue   = entry.clue ? std::string(entry.clue) : "";
        used_words.insert(word_str);

        if (backtrack(slots, grid, word_db, used_words, word_index))
            return true;

        for (int i = 0; i < (int)slot_cells.size(); i++) {
            auto [r, c] = slot_cells[i];
            grid[r][c] = saved[i];
        }
        best_slot->answer.clear();
        best_slot->clue.clear();
        used_words.erase(word_str);
    }

    return false;
}

std::optional<PuzzleGrid> generate_puzzle(
    const WordDB& word_db,
    const std::string& language,
    int max_attempts)
{
    std::vector<Pattern> valid_patterns;
    for (const auto& p : PATTERNS)
        if (is_valid_pattern(p)) valid_patterns.push_back(p);

    if (valid_patterns.empty()) {
        Serial.println("[ERROR] No valid patterns available");
        return std::nullopt;
    }

    WordIndex word_index = build_index(word_db);

    Serial.print("Heap after build_index: "); Serial.println(ESP.getFreeHeap());
    Serial.print("PSRAM after build_index: "); Serial.println(ESP.getFreePsram());

    std::uniform_int_distribution<int> dist(0, (int)valid_patterns.size() - 1);

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        yield();
        const Pattern& pattern = valid_patterns[dist(rng)];
        PuzzleGrid puzzle(pattern, language);
        std::set<std::string> used_words;

        if (backtrack(puzzle.slots, puzzle.grid, word_db, used_words, &word_index)) {
            Serial.print("[INFO] Puzzle generated (attempt ");
            Serial.print(attempt + 1);
            Serial.print("/");
            Serial.print(max_attempts);
            Serial.println(")");
            return puzzle;
        }
    }

    Serial.println("[ERROR] Generation failed after max attempts");
    return std::nullopt;
}

// =============================================================================
// SECTION 4 — CLUE MANAGEMENT
// =============================================================================

const WordSlot* get_active_clue(
    const PuzzleGrid& puzzle,
    int selected_row,
    int selected_col,
    const std::string& direction)
{
    for (const auto& slot : puzzle.slots) {
        if (slot.direction != direction) continue;
        for (const auto& [r, c] : slot.cells()) {
            if (r == selected_row && c == selected_col)
                return &slot;
        }
    }
    return nullptr;
}

// =============================================================================
// SECTION 5 — DISPLAY / OUTPUT
// =============================================================================

void print_grid(const PuzzleGrid& puzzle) {
    Serial.println();
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            char cell = puzzle.grid[r][c];
            if      (cell == BLACK_SQ) Serial.print("# ");
            else if (cell == EMPTY_SQ) Serial.print(". ");
            else { Serial.print(cell); Serial.print(" "); }
        }
        Serial.println();
    }
    Serial.println();
}

void print_clues(const PuzzleGrid& puzzle) {
    Serial.println("ACROSS:");
    int i = 1;
    for (const auto& slot : puzzle.slots) {
        if (slot.direction != "across") continue;
        Serial.print("  "); Serial.print(i++); Serial.print(". ");
        Serial.print(slot.clue.c_str()); Serial.print(" -> ");
        Serial.println(slot.answer.c_str());
    }
    Serial.println("DOWN:");
    i = 1;
    for (const auto& slot : puzzle.slots) {
        if (slot.direction != "down") continue;
        Serial.print("  "); Serial.print(i++); Serial.print(". ");
        Serial.print(slot.clue.c_str()); Serial.print(" -> ");
        Serial.println(slot.answer.c_str());
    }
}

void print_puzzle(const PuzzleGrid& puzzle) {
    Serial.println("=== 5x5 CROSSWORD ===");
    print_grid(puzzle);
    print_clues(puzzle);
}