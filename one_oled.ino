#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <LittleFS.h>
#include "algorithm.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define OLED_MOSI  23
#define OLED_CLK   18
#define OLED_DC    2
#define OLED_RESET 4

#define MUX_A0  27
#define MUX_A1  13
#define MUX_A2  14
#define MUX_A3  15
#define MUX_A4  5

#define ENCODER_CLK  25
#define ENCODER_DT   26
#define ENCODER_SW   70
#define MENU_BTN     33
#define NEXT_CLUE_BTN 19
#define SELECT_BTN 32

#define NUM_DISPLAYS 25
#define BLINK_INTERVAL 500
#define MUX_OFFSET 0

// LCD geometry
#define LCD_COLS 20
#define LCD_ROWS 4

// AUTOTEST CONFIGURATION
// =============================================================================
// Set AUTOTEST_ENABLED to true to run the endurance test at boot. When the
// test finishes, execution falls through to the normal main menu so the
// device is immediately usable. Set back to false for the live demo run.
#define AUTOTEST_ENABLED false
#define AUTOTEST_TRIALS  60

// Exposed by algorithm.cpp: set to (attempt + 1) when generate_puzzle succeeds.
extern int last_attempt_count;

// STATE MACHINE
enum AppState {
    STATE_MENU,
    STATE_LANGUAGE_MENU,
    STATE_CHECK_MENU,
    STATE_GENERATING,
    STATE_PLAYING,
    STATE_GAME_MENU,
    STATE_WIN
};

AppState appState = STATE_MENU;

// Main menu
const char* menuItems[] = { "Generate Puzzle", "Language", "Answer Check" };
const int   NUM_MENU_ITEMS = 3;
int  menuIndex   = 0;
bool menuChanged = true;

// In-game menu
const char* gameMenuItems[] = { "Resume", "Generate Puzzle", "Language", "Answer Check" };
const int NUM_GAME_MENU_ITEMS = 4;
int gameMenuIndex = 0;

// Language menu
const char* languageItems[] = { "English", "Spanish", "German"};
const char* languageCodes[] = { "english", "spanish", "german"};
const int NUM_LANGUAGES = 3;
int languageIndex = 0;
int selectedLanguage = 0;

// Remember which menu opened a submenu so we can return
AppState languageReturnState = STATE_MENU;
AppState checkReturnState    = STATE_MENU;

// ANSWER CHECK MODES
enum CheckMode {
    CHECK_PUZZLE  = 0,
    CHECK_EACH    = 1,
    CHECK_GIVE_UP = 2
};

const char* checkMenuItems[] = { "Check Puzzle", "Check Word Cells", "Give Up" };
const int NUM_CHECK_MENU_ITEMS = 3;
int checkMenuIndex = 0;
CheckMode selectedCheckMode = CHECK_PUZZLE;  // default

// Tracks which displays have been marked as wrong
bool cellWrong[NUM_DISPLAYS];

// Puzzle
std::optional<PuzzleGrid> currentPuzzle;
WordDB word_db;
bool dictLoaded = false;

// Clue ordering
std::vector<int> clueOrder;
int currentClueIndex = 0;

// HARDWARE
LiquidCrystal_I2C lcd(0x27, LCD_COLS, LCD_ROWS);

Adafruit_SSD1306* displays[NUM_DISPLAYS];
bool displayActive[NUM_DISPLAYS];

const int CONNECTED_DISPLAYS[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};
const int NUM_CONNECTED = sizeof(CONNECTED_DISPLAYS) / sizeof(CONNECTED_DISPLAYS[0]);

uint8_t currentDisplay = 0;
char    currentLetter  = 'A';
char    grid[NUM_DISPLAYS];

volatile int           encoderCount      = 0;
volatile unsigned long lastInterruptTime = 0;
int                    lastEncoderCount  = 0;

bool          cursorVisible = true;
unsigned long lastBlinkTime = 0;

// Mux settle delay in ms. Normal operation uses 5 ms (clear) + 10 ms (set).
// The autotest sweeps these to characterize the reliability/speed trade-off.
volatile int muxClearDelayMs = 5;
volatile int muxSetDelayMs   = 10;

// MEMORY INSTRUMENTATION
void logMemory(const char* label) {
    uint32_t heap      = ESP.getFreeHeap();
    uint32_t heapMin   = ESP.getMinFreeHeap();
    uint32_t heapSize  = ESP.getHeapSize();
    uint32_t psram     = ESP.getFreePsram();
    uint32_t psramSize = ESP.getPsramSize();

    Serial.print("[MEM] ");
    Serial.print(label);
    Serial.print(" | heap=");
    Serial.print(heap);
    Serial.print("/");
    Serial.print(heapSize);
    Serial.print(" (");
    Serial.print((100.0f * (heapSize - heap)) / heapSize, 1);
    Serial.print("% used)");
    Serial.print(" minEverHeap=");
    Serial.print(heapMin);
    Serial.print(" psram=");
    Serial.print(psram);
    Serial.print("/");
    Serial.println(psramSize);
}

size_t totalWordBytes() {
    size_t bytes = 0;
    for (auto& [len, entries] : word_db) {
        bytes += sizeof(DictEntry) * entries.size();
        for (auto& e : entries) {
            if (e.word) bytes += strlen(e.word) + 1;
            if (e.clue) bytes += strlen(e.clue) + 1;
        }
    }
    return bytes;
}

// ISR
void IRAM_ATTR encoderISR() {
    unsigned long now = millis();
    if (now - lastInterruptTime < 150) return;
    lastInterruptTime = now;
    if (digitalRead(ENCODER_DT) != digitalRead(ENCODER_CLK))
        encoderCount++;
    else
        encoderCount--;
}

// MUX + OLED HELPERS
void selectDisplay(uint8_t index) {
    uint8_t ch = index + MUX_OFFSET;
    // Park the mux at an unused channel (25-31) during the settle delay
    // so we don't briefly route SPI to display 0. Channel 31 = all pins HIGH.
    digitalWrite(MUX_A0, HIGH); digitalWrite(MUX_A1, HIGH);
    digitalWrite(MUX_A2, HIGH); digitalWrite(MUX_A3, HIGH);
    digitalWrite(MUX_A4, HIGH);
    if (muxClearDelayMs > 0) delay(muxClearDelayMs);
    digitalWrite(MUX_A0, (ch >> 0) & 1);
    digitalWrite(MUX_A1, (ch >> 1) & 1);
    digitalWrite(MUX_A2, (ch >> 2) & 1);
    digitalWrite(MUX_A3, (ch >> 3) & 1);
    digitalWrite(MUX_A4, (ch >> 4) & 1);
    if (muxSetDelayMs > 0) delay(muxSetDelayMs);
}

void showLetter(uint8_t index, char letter, bool showCursor) {
    if (index >= NUM_DISPLAYS || !displayActive[index]) return;

    selectDisplay(index);
    Adafruit_SSD1306* d = displays[index];
    d->begin(SSD1306_SWITCHCAPVCC);
    delay(5);
    d->clearDisplay();
    d->setRotation(3);
    d->setTextColor(WHITE);
    d->setTextSize(8);
    // Center the 48x64 glyph (size 8) in the 64x128 rotated frame:
    //   x = (64 - 48) / 2 = 8
    //   y = (128 - 64) / 2 = 32
    d->setCursor(8, 32);
    d->print(letter);
    if (showCursor) {
        // Cursor underline — centered under the letter, near bottom
        d->setTextSize(2);
        d->setCursor(20, 85);
        d->print("_");
    }
    if (cellWrong[index]) {
        d->drawLine(0,  0,  63, 127, WHITE);
        d->drawLine(63, 0,  0,  127, WHITE);
        d->drawLine(0,  1,  62, 127, WHITE);
        d->drawLine(62, 1,  0,  126, WHITE);
    }
    d->display();
}

void showLetterWithCheckmark(uint8_t index, char letter) {
    if (index >= NUM_DISPLAYS || !displayActive[index]) return;

    selectDisplay(index);
    Adafruit_SSD1306* d = displays[index];
    d->begin(SSD1306_SWITCHCAPVCC);
    delay(5);
    d->clearDisplay();
    d->setRotation(3);
    d->setTextColor(WHITE);
    d->setTextSize(8);
    d->setCursor(8, 32);  // centered (see showLetter comment)
    d->print(letter);

    // Checkmark in the top-right corner
    int cx = 55;
    int cy = 5;
    d->drawLine(cx - 6, cy + 2,  cx - 3, cy + 6, WHITE);
    d->drawLine(cx - 3, cy + 6,  cx + 3, cy - 3, WHITE);
    d->drawLine(cx - 6, cy + 3,  cx - 3, cy + 7, WHITE);
    d->drawLine(cx - 3, cy + 7,  cx + 3, cy - 2, WHITE);

    d->display();
}

void clearOLED(uint8_t index) {
    if (index >= NUM_DISPLAYS || !displayActive[index]) return;
    selectDisplay(index);
    Adafruit_SSD1306* d = displays[index];
    d->clearDisplay();
    d->display();
}

// LCD HELPERS (20x4)
void lcdPrintRow(int row, const char* text) {
    lcd.setCursor(0, row);
    int len = strlen(text);
    if (len > LCD_COLS) len = LCD_COLS;
    for (int i = 0; i < len; i++) lcd.print(text[i]);
    for (int i = len; i < LCD_COLS; i++) lcd.print(' ');
}

void lcdPrint(const char* line0, const char* line1 = "") {
    lcd.clear();
    lcdPrintRow(0, line0);
    lcdPrintRow(1, line1);
}

void drawList(const char* title, const char* const* items, int count, int selected) {
    lcd.clear();
    const int visibleRows = LCD_ROWS;

    int startIdx = 0;
    if (count > visibleRows) {
        if (selected >= visibleRows) startIdx = selected - (visibleRows - 1);
        if (startIdx + visibleRows > count) startIdx = count - visibleRows;
        if (startIdx < 0) startIdx = 0;
    }

    for (int row = 0; row < visibleRows; row++) {
        int itemIdx = startIdx + row;
        if (itemIdx >= count) { lcdPrintRow(row, ""); continue; }
        char buf[LCD_COLS + 1];
        snprintf(buf, sizeof(buf), "%c%s",
                 (itemIdx == selected) ? '>' : ' ',
                 items[itemIdx]);
        lcdPrintRow(row, buf);
    }
}

void drawMainMenu()     { drawList("main", menuItems, NUM_MENU_ITEMS, menuIndex); }
void drawGameMenu()     { drawList("gam", gameMenuItems, NUM_GAME_MENU_ITEMS, gameMenuIndex); }
void drawLanguageMenu() { drawList("lang", languageItems, NUM_LANGUAGES, languageIndex); }
void drawCheckMenu()    { drawList("check", checkMenuItems, NUM_CHECK_MENU_ITEMS, checkMenuIndex); }

// CLUE DISPLAY
void buildClueOrder() {
    clueOrder.clear();
    if (!currentPuzzle) return;
    for (int i = 0; i < (int)currentPuzzle->slots.size(); i++)
        if (currentPuzzle->slots[i].direction == "across") clueOrder.push_back(i);
    for (int i = 0; i < (int)currentPuzzle->slots.size(); i++)
        if (currentPuzzle->slots[i].direction == "down") clueOrder.push_back(i);
    currentClueIndex = 0;
}

int getClueNumber(const PuzzleGrid& puzzle, int slotIndex) {
    const WordSlot& target = puzzle.slots[slotIndex];
    int num = 0;
    for (int i = 0; i <= slotIndex; i++)
        if (puzzle.slots[i].direction == target.direction) num++;
    return num;
}

bool slotContainsCell(const WordSlot& slot, int r, int c) {
    for (const Cell& cell : slot.cells()) {
        if (cell.first == r && cell.second == c) return true;
    }
    return false;
}

void wrapText(const char* text, char rows[][LCD_COLS + 1], int maxRows) {
    for (int i = 0; i < maxRows; i++) rows[i][0] = '\0';
    int len = strlen(text);
    int pos = 0, rowIdx = 0;
    while (pos < len && rowIdx < maxRows) {
        int remaining = len - pos;
        if (remaining <= LCD_COLS) {
            strncpy(rows[rowIdx], text + pos, LCD_COLS);
            rows[rowIdx][LCD_COLS] = '\0';
            pos = len; rowIdx++; break;
        }
        int splitAt = LCD_COLS;
        for (int i = LCD_COLS; i > 0; i--) {
            if (text[pos + i] == ' ') { splitAt = i; break; }
        }
        if (splitAt == 0 || splitAt == LCD_COLS) splitAt = LCD_COLS;
        strncpy(rows[rowIdx], text + pos, splitAt);
        rows[rowIdx][splitAt] = '\0';
        pos += splitAt;
        while (pos < len && text[pos] == ' ') pos++;
        rowIdx++;
    }
}

void showNumberedClue(int clueIdx) {
    if (!currentPuzzle || clueIdx < 0 || clueIdx >= (int)clueOrder.size()) return;
    int slotIdx = clueOrder[clueIdx];
    const WordSlot& slot = currentPuzzle->slots[slotIdx];
    int num = getClueNumber(*currentPuzzle, slotIdx);
    char dirChar = (slot.direction == "across") ? 'A' : 'D';

    char header[LCD_COLS + 1];
    snprintf(header, sizeof(header), "%d%c  (%d of %d)",
             num, dirChar, clueIdx + 1, (int)clueOrder.size());

    char wrapped[3][LCD_COLS + 1];
    wrapText(slot.clue.c_str(), wrapped, 3);

    lcd.clear();
    lcdPrintRow(0, header);
    lcdPrintRow(1, wrapped[0]);
    lcdPrintRow(2, wrapped[1]);
    lcdPrintRow(3, wrapped[2]);
}

void showActiveClue() {
    if (!currentPuzzle || clueOrder.empty()) return;
    showNumberedClue(currentClueIndex);
}

void moveCursorToCurrentClue() {
    if (!currentPuzzle || clueOrder.empty()) return;

    // Clean up the old cursor position before moving. Preserve black-square
    // white fills instead of blanking them.
    if (currentDisplay < NUM_DISPLAYS && displayActive[currentDisplay]) {
        int oldR = currentDisplay / 5;
        int oldC = currentDisplay % 5;
        bool oldIsBlack = (currentPuzzle->grid[oldR][oldC] == BLACK_SQ);

        if (oldIsBlack) {
            // Black square — keep it white, don't blank it
            selectDisplay(currentDisplay);
            displays[currentDisplay]->begin(SSD1306_SWITCHCAPVCC);
            delay(5);
            displays[currentDisplay]->fillScreen(WHITE);
            displays[currentDisplay]->display();
        } else {
            char oldLetter = grid[currentDisplay];
            if (oldLetter != 0) {
                showLetter(currentDisplay, oldLetter, false);
            } else {
                clearOLED(currentDisplay);
            }
        }
    }

    int slotIdx = clueOrder[currentClueIndex];
    const WordSlot& slot = currentPuzzle->slots[slotIdx];
    int displayIdx = slot.start_row * 5 + slot.start_col;
    currentDisplay = displayIdx;

    encoderCount = 0;
    lastEncoderCount = 0;

    if (currentDisplay < NUM_DISPLAYS && grid[currentDisplay] != 0) {
        currentLetter = grid[currentDisplay];
        encoderCount = currentLetter - 'A';
        lastEncoderCount = encoderCount;
    } else {
        currentLetter = 'A';
    }

    cursorVisible = true;
    lastBlinkTime = millis();

    if (currentDisplay < NUM_DISPLAYS && displayActive[currentDisplay]) {
        showLetter(currentDisplay, currentLetter, true);
    }
    Serial.print("Cursor moved to display "); Serial.println(currentDisplay);
}

// LITTLEFS / DICTIONARY
void testDictionary() {
    File f = LittleFS.open("/dictionaries/german_3.txt", "r");
    if (!f) {
        Serial.println("TEST FAIL: could not open german_3.txt");
        lcdPrint("Dict test FAIL", "File not found");
        return;
    }
    String line = f.readStringUntil('\n');
    f.close();
    Serial.print("TEST PASS: first line = "); Serial.println(line);
    lcdPrint("Dict OK:", line.c_str());
}

void freeDictionary() {
    for (auto& [len, entries] : word_db) {
        for (auto& e : entries) {
            if (e.word) { free(e.word); e.word = nullptr; }
            if (e.clue) { free(e.clue); e.clue = nullptr; }
        }
    }
    word_db.clear();
    dictLoaded = false;
}

bool loadDictionary(const std::string& language) {
    lcdPrint("Loading dict...", language.c_str());
    logMemory("before dict load");

    for (int len : SUPPORTED_LENGTHS) {
        std::string filepath = "/dictionaries/" + language + "_" + std::to_string(len) + ".txt";
        File file = LittleFS.open(filepath.c_str(), "r");
        if (!file) {
            Serial.print("File not found: "); Serial.println(filepath.c_str());
            return false;
        }

        int count = 0;
        while (file.available()) {
            String raw = file.readStringUntil('\n');
            raw.trim();
            if (raw.length() > 0 && raw[0] != '#' && raw.indexOf(',') >= 0) count++;
        }
        file.seek(0);
        word_db[len].reserve(count);

        int loaded = 0;
        while (file.available()) {
            String raw = file.readStringUntil('\n');
            raw.trim();
            if (raw.length() == 0 || raw[0] == '#') continue;
            int comma = raw.indexOf(',');
            if (comma < 0) continue;
            String wordStr = raw.substring(0, comma);
            wordStr.trim();
            wordStr.toUpperCase();
            if ((int)wordStr.length() != len) continue;

            DictEntry entry;
            entry.word = strdup(wordStr.c_str());
            entry.clue = nullptr;
            word_db[len].push_back(entry);
            loaded++;
            if (loaded % 200 == 0) yield();
        }
        file.close();

        Serial.print("[DICT] Loaded "); Serial.print(loaded);
        Serial.print(" words of length "); Serial.println(len);
        char label[40];
        snprintf(label, sizeof(label), "after length-%d loaded", len);
        logMemory(label);
    }

    dictLoaded = true;
    Serial.print("Dictionary loaded successfully; total bytes = ");
    Serial.println((unsigned long)totalWordBytes());
    logMemory("dict fully loaded");
    return true;
}

String lookupClue(const std::string& language, const std::string& word) {
    int len = word.length();
    std::string filepath = "/dictionaries/" + language + "_" + std::to_string(len) + ".txt";
    File file = LittleFS.open(filepath.c_str(), "r");
    if (!file) return "???";

    while (file.available()) {
        String raw = file.readStringUntil('\n');
        raw.trim();
        if (raw.length() == 0 || raw[0] == '#') continue;
        int comma = raw.indexOf(',');
        if (comma < 0) continue;
        String wordStr = raw.substring(0, comma);
        wordStr.trim();
        wordStr.toUpperCase();

        if (wordStr.length() == (unsigned)len) {
            bool match = true;
            for (int i = 0; i < len; i++) {
                if (wordStr[i] != word[i]) { match = false; break; }
            }
            if (match) {
                file.close();
                String clueStr = raw.substring(comma + 1);
                clueStr.trim();
                return clueStr;
            }
        }
    }
    file.close();
    return "???";
}

void fillCluesFromFile(PuzzleGrid& puzzle, const std::string& language) {
    Serial.println("Looking up clues from LittleFS...");
    for (auto& slot : puzzle.slots) {
        if (slot.answer.empty()) continue;
        String clue = lookupClue(language, slot.answer);
        slot.clue = std::string(clue.c_str());
        Serial.print("  "); Serial.print(slot.answer.c_str());
        Serial.print(" -> "); Serial.println(slot.clue.c_str());
    }
}

// ANSWER CHECK HELPERS
bool isPuzzleFullyFilled() {
    if (!currentPuzzle) return false;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (currentPuzzle->grid[r][c] == BLACK_SQ) continue;
            int idx = r * 5 + c;
            if (grid[idx] == 0) return false;
        }
    }
    return true;
}

bool isSlotFullyFilled(const WordSlot& slot) {
    for (const Cell& cell : slot.cells()) {
        int idx = cell.first * 5 + cell.second;
        if (grid[idx] == 0) return false;
    }
    return true;
}

void markCellWrong(int idx, bool wrong) {
    if (idx < 0 || idx >= NUM_DISPLAYS) return;
    cellWrong[idx] = wrong;
    if (displayActive[idx] && grid[idx] != 0) {
        showLetter(idx, grid[idx], false);
    }
}

void clearAllWrongFlags() {
    for (int i = 0; i < NUM_DISPLAYS; i++) cellWrong[i] = false;
}

int checkSlotCells(const WordSlot& slot) {
    int wrongCount = 0;
    auto cells = slot.cells();
    for (int i = 0; i < (int)cells.size() && i < (int)slot.answer.size(); i++) {
        int idx = cells[i].first * 5 + cells[i].second;
        char expected = slot.answer[i];
        char actual   = grid[idx];
        if (actual != 0 && actual != expected) {
            markCellWrong(idx, true);
            wrongCount++;
        } else {
            markCellWrong(idx, false);
        }
    }
    return wrongCount;
}

int checkWholePuzzle() {
    if (!currentPuzzle) return 0;
    int wrongCount = 0;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            char expected = currentPuzzle->grid[r][c];
            if (expected == BLACK_SQ) continue;
            int idx = r * 5 + c;
            char actual = grid[idx];
            if (actual != 0 && actual != expected) {
                markCellWrong(idx, true);
                wrongCount++;
            } else {
                markCellWrong(idx, false);
            }
        }
    }
    return wrongCount;
}

void revealSolution() {
    if (!currentPuzzle) return;
    clearAllWrongFlags();
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int idx = r * 5 + c;
            if (!displayActive[idx]) continue;
            char cell = currentPuzzle->grid[r][c];
            if (cell == BLACK_SQ) {
                selectDisplay(idx);
                displays[idx]->begin(SSD1306_SWITCHCAPVCC);
                delay(5);
                displays[idx]->fillScreen(WHITE);
                displays[idx]->display();
            } else if (cell != EMPTY_SQ) {
                grid[idx] = cell;
                showLetter(idx, cell, false);
            }
        }
    }
}

// WIN SCREEN
void enterWinScreen() {
    if (!currentPuzzle) return;
    Serial.println("=== PUZZLE WIN ===");

    lcd.clear();
    lcdPrintRow(0, "                    ");
    lcdPrintRow(1, "  Puzzle Complete!  ");
    lcdPrintRow(2, "    All correct!    ");
    lcdPrintRow(3, "                    ");

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int idx = r * 5 + c;
            if (!displayActive[idx]) continue;
            char cell = currentPuzzle->grid[r][c];
            if (cell == BLACK_SQ) continue;
            if (grid[idx] != 0) {
                showLetterWithCheckmark(idx, grid[idx]);
            }
        }
    }

    appState = STATE_WIN;
}

void maybeCheckFilledSlots(int editedIdx) {
    if (!currentPuzzle || selectedCheckMode != CHECK_EACH) return;
    int r = editedIdx / 5;
    int c = editedIdx % 5;
    for (const auto& slot : currentPuzzle->slots) {
        if (!slotContainsCell(slot, r, c)) continue;
        if (isSlotFullyFilled(slot)) {
            checkSlotCells(slot);
        }
    }
    if (isPuzzleFullyFilled()) {
        int wrongTotal = 0;
        for (int i = 0; i < NUM_DISPLAYS; i++) if (cellWrong[i]) wrongTotal++;
        if (wrongTotal == 0) {
            enterWinScreen();
        }
    }
}

void maybeCheckFullPuzzle() {
    if (!currentPuzzle || selectedCheckMode != CHECK_PUZZLE) return;
    if (!isPuzzleFullyFilled()) return;
    int wrongCount = checkWholePuzzle();
    if (wrongCount == 0) {
        enterWinScreen();
    } else {
        char buf[LCD_COLS + 1];
        snprintf(buf, sizeof(buf), "%d wrong cell%s",
                 wrongCount, wrongCount == 1 ? "" : "s");
        lcdPrint("Puzzle filled:", buf);
        delay(2000);

        // Jump cursor to the first wrong cell (lowest display index) so
        // the user can immediately correct it
        int firstWrong = -1;
        for (int i = 0; i < NUM_DISPLAYS; i++) {
            if (cellWrong[i]) { firstWrong = i; break; }
        }
        if (firstWrong >= 0) {
            // Redraw the cell the cursor is leaving (without cursor)
            if (currentDisplay < NUM_DISPLAYS && displayActive[currentDisplay] &&
                currentDisplay != (uint8_t)firstWrong) {
                showLetter(currentDisplay, grid[currentDisplay], false);
            }

            // Snap the cursor to the first wrong cell
            currentDisplay = firstWrong;

            // Match the current letter to what's in the grid
            if (grid[currentDisplay] != 0) {
                currentLetter = grid[currentDisplay];
                encoderCount = currentLetter - 'A';
                lastEncoderCount = encoderCount;
            } else {
                currentLetter = 'A';
                encoderCount = 0;
                lastEncoderCount = 0;
            }

            // Also update the LCD to show the clue that owns this cell.
            // Prefer an across clue if the wrong cell is part of one;
            // otherwise fall back to the down clue.
            if (!clueOrder.empty()) {
                int r = currentDisplay / 5;
                int c = currentDisplay % 5;
                int acrossIdx = -1, downIdx = -1;
                for (int i = 0; i < (int)clueOrder.size(); i++) {
                    const WordSlot& slot = currentPuzzle->slots[clueOrder[i]];
                    if (slotContainsCell(slot, r, c)) {
                        if (slot.direction == "across" && acrossIdx < 0) acrossIdx = i;
                        else if (slot.direction == "down" && downIdx < 0) downIdx = i;
                    }
                }
                currentClueIndex = (acrossIdx >= 0) ? acrossIdx
                                 : (downIdx   >= 0) ? downIdx
                                 : currentClueIndex;
            }

            cursorVisible = true;
            lastBlinkTime = millis();
            if (displayActive[currentDisplay]) {
                showLetter(currentDisplay, currentLetter, true);
            }
            showActiveClue();

            Serial.print("Cursor snapped to first wrong cell: ");
            Serial.println(currentDisplay);
        } else {
            showActiveClue();
        }
    }
}

// PUZZLE GENERATION
void startGeneration() {
    appState = STATE_GENERATING;

    const char* langCode = languageCodes[selectedLanguage];
    char subtitle[LCD_COLS + 1];
    snprintf(subtitle, sizeof(subtitle), "Lang: %s", languageItems[selectedLanguage]);
    lcd.clear();
    lcdPrintRow(0, "Generating...");
    lcdPrintRow(1, subtitle);
    lcdPrintRow(2, "Please wait");
    lcdPrintRow(3, "");

    if (dictLoaded) {
        freeDictionary();
        logMemory("after old dict freed");
    }

    if (!loadDictionary(langCode)) {
        lcdPrint("Dict not found", langCode);
        delay(2000);
        appState = STATE_MENU;
        menuChanged = true;
        return;
    }

    logMemory("before generate_puzzle");
    unsigned long genStart = millis();

    currentPuzzle = generate_puzzle(word_db, langCode, 20);

    unsigned long genElapsed = millis() - genStart;
    Serial.print("[TIMING] generate_puzzle took ");
    Serial.print(genElapsed);
    Serial.println(" ms");
    logMemory("after generate_puzzle");

    if (!currentPuzzle) {
        lcdPrint("Gen failed :(", "Try again");
        delay(2000);
        appState = STATE_MENU;
        menuChanged = true;
        return;
    }

    freeDictionary();
    Serial.println("Dictionary freed");
    logMemory("after dict freed");

    fillCluesFromFile(*currentPuzzle, langCode);
    logMemory("after clues filled");
    buildClueOrder();

    Serial.println("Re-initializing displays...");
    for (int j = 0; j < NUM_CONNECTED; j++) {
        int i = CONNECTED_DISPLAYS[j];
        if (displays[i]) {
            selectDisplay(i);
            displays[i]->begin(SSD1306_SWITCHCAPVCC);
            delay(10);
        }
    }
    delay(100);

    currentDisplay = 0;
    currentLetter  = 'A';
    memset(grid, 0, sizeof(grid));
    clearAllWrongFlags();
    encoderCount     = 0;
    lastEncoderCount = 0;
    cursorVisible    = true;
    lastBlinkTime    = millis();

    digitalWrite(OLED_RESET, HIGH);
    delay(10);

    if (currentPuzzle) {
        Serial.println("Displaying puzzle on OLEDs...");
        for (int r = 0; r < GRID_SIZE; r++) {
            for (int c = 0; c < GRID_SIZE; c++) {
                int idx = r * 5 + c;
                if (!displayActive[idx]) continue;
                char cell = currentPuzzle->grid[r][c];
                selectDisplay(idx);
                displays[idx]->begin(SSD1306_SWITCHCAPVCC);
                delay(10);

                if (cell == BLACK_SQ) {
                    displays[idx]->fillScreen(WHITE);
                    displays[idx]->display();
                    delay(10);
                } else {
                    displays[idx]->clearDisplay();
                    displays[idx]->display();
                }
            }
        }
    }

    showActiveClue();
    moveCursorToCurrentClue();
    print_puzzle(*currentPuzzle);

    appState = STATE_PLAYING;
    logMemory("puzzle ready (steady state)");
    Serial.println("Puzzle ready");
}

// RESUME HELPER
void resumeGame() {
    encoderCount = 0;
    lastEncoderCount = 0;
    if (currentDisplay < NUM_DISPLAYS && grid[currentDisplay] != 0) {
        currentLetter = grid[currentDisplay];
        encoderCount = currentLetter - 'A';
        lastEncoderCount = encoderCount;
    } else {
        currentLetter = 'A';
    }
    cursorVisible = true;
    lastBlinkTime = millis();
    if (currentDisplay < NUM_DISPLAYS && displayActive[currentDisplay]) {
        showLetter(currentDisplay, currentLetter, true);
    }
    showActiveClue();
    appState = STATE_PLAYING;
    Serial.println("Resumed game");
}

// SUBMENU OPENERS
void openLanguageMenu(AppState returnTo) {
    languageReturnState = returnTo;
    languageIndex = selectedLanguage;
    encoderCount = 0;
    lastEncoderCount = 0;
    appState = STATE_LANGUAGE_MENU;
    drawLanguageMenu();
}

void openCheckMenu(AppState returnTo) {
    checkReturnState = returnTo;
    checkMenuIndex = (int)selectedCheckMode;
    encoderCount = 0;
    lastEncoderCount = 0;
    appState = STATE_CHECK_MENU;
    drawCheckMenu();
}

void returnAfterCheckSelection() {
    encoderCount = 0;
    lastEncoderCount = 0;
    if (checkReturnState == STATE_GAME_MENU || checkReturnState == STATE_PLAYING) {
        resumeGame();
    } else {
        appState = STATE_MENU;
        menuChanged = true;
    }
}

// DISPLAY INIT
void initDisplays() {
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, HIGH); delay(10);
    digitalWrite(OLED_RESET, LOW);  delay(10);
    digitalWrite(OLED_RESET, HIGH); delay(10);

    for (int i = 0; i < NUM_DISPLAYS; i++) {
        displays[i] = nullptr;
        displayActive[i] = false;
        cellWrong[i] = false;
    }

    for (int j = 0; j < NUM_CONNECTED; j++) {
        int i = CONNECTED_DISPLAYS[j];
        displays[i] = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT,
            OLED_MOSI, OLED_CLK, OLED_DC, -1, -1);

        selectDisplay(i);
        if (displays[i]->begin(SSD1306_SWITCHCAPVCC)) {
            displays[i]->clearDisplay();
            displays[i]->display();
            displayActive[i] = true;
            Serial.print("Display "); Serial.print(i); Serial.println(" OK");
        } else {
            Serial.print("Display "); Serial.print(i); Serial.println(" FAILED");
            delete displays[i];
            displays[i] = nullptr;
        }
        delay(50);
    }

    int count = 0;
    for (int i = 0; i < NUM_DISPLAYS; i++) if (displayActive[i]) count++;
    Serial.print("Active displays: "); Serial.println(count);
}

// AUTOTEST — puzzle generation endurance test
// Runs AUTOTEST_TRIALS consecutive puzzle generations, cycling through all
// installed languages. Measures per-trial success, time, attempt count, and
// tracks heap drift across the full run to detect memory leaks in the
// load/free dictionary cycle.
//
// This is the quantitative test for the final prototype demonstration.
void runAutoTest() {
    const int N = AUTOTEST_TRIALS;

    Serial.println("\n========================================");
    Serial.print("AUTOTEST — "); Serial.print(N); Serial.println(" PUZZLE GENERATIONS");
    Serial.print("Languages under test: ");
    for (int i = 0; i < NUM_LANGUAGES; i++) {
        Serial.print(languageItems[i]);
        if (i < NUM_LANGUAGES - 1) Serial.print(", ");
    }
    Serial.println();
    Serial.println("========================================\n");

    lcd.clear();
    lcdPrintRow(0, "AUTOTEST: PUZZLES");
    lcdPrintRow(1, "See serial monitor");

    unsigned long times[N];
    int           attempts[N];
    bool          success[N];
    int           langOfTrial[N];

    unsigned long testStart = millis();
    uint32_t heapBefore = ESP.getMinFreeHeap();

    int successCount = 0;
    int failCount    = 0;

    for (int i = 0; i < N; i++) {
        // Rotate through the installed languages
        selectedLanguage = i % NUM_LANGUAGES;
        langOfTrial[i]   = selectedLanguage;
        const char* langCode = languageCodes[selectedLanguage];

        char lcdLine[LCD_COLS + 1];
        snprintf(lcdLine, sizeof(lcdLine), "Trial %d/%d %s",
                 i + 1, N, languageItems[selectedLanguage]);
        lcdPrintRow(2, lcdLine);
        lcdPrintRow(3, "                    ");

        // Free any previous dictionary to mirror normal gameplay flow
        if (dictLoaded) freeDictionary();

        if (!loadDictionary(langCode)) {
            Serial.print("[TEST ");
            Serial.print(i + 1);
            Serial.println("] Dictionary load FAILED");
            times[i] = 0;
            attempts[i] = 0;
            success[i] = false;
            failCount++;
            continue;
        }

        // Run the timed generation
        last_attempt_count = 0;
        unsigned long t0 = millis();
        std::optional<PuzzleGrid> result = generate_puzzle(word_db, langCode, 20);
        unsigned long elapsed = millis() - t0;

        bool ok = result.has_value();
        times[i]    = elapsed;
        attempts[i] = last_attempt_count;
        success[i]  = ok;

        if (ok) successCount++; else failCount++;

        Serial.print("[TEST ");
        Serial.print(i + 1);
        Serial.print("/");
        Serial.print(N);
        Serial.print("] lang=");
        Serial.print(languageItems[selectedLanguage]);
        Serial.print(" success=");
        Serial.print(ok ? "YES" : "NO");
        Serial.print(" time=");
        Serial.print(elapsed);
        Serial.print("ms attempts=");
        Serial.print(last_attempt_count);
        Serial.print(" minEverHeap=");
        Serial.println(ESP.getMinFreeHeap());

        // LCD status line so you can watch progress
        snprintf(lcdLine, sizeof(lcdLine), "%s %lu ms",
                 ok ? "OK" : "FAIL", (unsigned long)elapsed);
        lcdPrintRow(3, lcdLine);

        // Free the dictionary at end of trial to mirror production flow
        freeDictionary();
        yield();
    }

    unsigned long totalTime = millis() - testStart;
    uint32_t heapAfter = ESP.getMinFreeHeap();

    // Overall statistics (across all successful trials)
    unsigned long sumT = 0, minT = 0xFFFFFFFF, maxT = 0;
    int sumA = 0, minA = INT_MAX, maxA = 0;
    int okN = 0;
    for (int i = 0; i < N; i++) {
        if (!success[i]) continue;
        sumT += times[i];
        if (times[i] < minT) minT = times[i];
        if (times[i] > maxT) maxT = times[i];
        sumA += attempts[i];
        if (attempts[i] < minA) minA = attempts[i];
        if (attempts[i] > maxA) maxA = attempts[i];
        okN++;
    }

    Serial.println("\n========= AUTOTEST SUMMARY =========");
    Serial.print("Trials run:         "); Serial.println(N);
    Serial.print("Successes:          "); Serial.println(successCount);
    Serial.print("Failures:           "); Serial.println(failCount);
    Serial.print("Overall success %:  ");
    Serial.print((100.0f * successCount) / N, 1);
    Serial.println("%");

    if (okN > 0) {
        Serial.print("Time (ms) avg/min/max:      ");
        Serial.print(sumT / okN); Serial.print(" / ");
        Serial.print(minT); Serial.print(" / "); Serial.println(maxT);
        Serial.print("Attempts avg/min/max:       ");
        Serial.print((float)sumA / okN, 2); Serial.print(" / ");
        Serial.print(minA); Serial.print(" / "); Serial.println(maxA);
    }

    // Per-language breakdown
    Serial.println("\nPer-language breakdown:");
    for (int L = 0; L < NUM_LANGUAGES; L++) {
        int langOk = 0, langTotal = 0;
        unsigned long langSumT = 0;
        int langSumA = 0;
        for (int i = 0; i < N; i++) {
            if (langOfTrial[i] != L) continue;
            langTotal++;
            if (success[i]) {
                langOk++;
                langSumT += times[i];
                langSumA += attempts[i];
            }
        }
        Serial.print("  ");
        Serial.print(languageItems[L]);
        Serial.print(": ");
        Serial.print(langOk); Serial.print("/"); Serial.print(langTotal);
        if (langOk > 0) {
            Serial.print("  avg time=");
            Serial.print(langSumT / langOk);
            Serial.print("ms  avg attempts=");
            Serial.print((float)langSumA / langOk, 2);
        }
        Serial.println();
    }

    Serial.print("\nTotal test duration: ");
    Serial.print(totalTime / 1000.0f, 1);
    Serial.println(" s");
    Serial.print("minEverHeap before: "); Serial.println(heapBefore);
    Serial.print("minEverHeap after:  "); Serial.println(heapAfter);
    Serial.print("Heap drift:         ");
    Serial.print((int32_t)heapAfter - (int32_t)heapBefore);
    Serial.println(" bytes (negative = heap got tighter)");
    Serial.println("====================================\n");

    // LCD summary
    lcd.clear();
    char row1[LCD_COLS + 1];
    snprintf(row1, sizeof(row1), "Autotest: %d/%d OK", successCount, N);
    lcdPrintRow(0, "AUTOTEST COMPLETE");
    lcdPrintRow(1, row1);
    if (okN > 0) {
        char row2[LCD_COLS + 1];
        snprintf(row2, sizeof(row2), "Avg time: %lu ms", (unsigned long)(sumT / okN));
        lcdPrintRow(2, row2);
        char row3[LCD_COLS + 1];
        snprintf(row3, sizeof(row3), "Avg attempts: %.2f", (float)sumA / okN);
        lcdPrintRow(3, row3);
    }
    delay(5000);
}

// SETUP
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n======= BOOT =======");
    if (psramInit()) {
        Serial.println("PSRAM initialized");
    } else {
        Serial.println("PSRAM not available");
    }
    logMemory("boot");

    pinMode(MUX_A0, OUTPUT); digitalWrite(MUX_A0, HIGH);
    pinMode(MUX_A1, OUTPUT); digitalWrite(MUX_A1, HIGH);
    pinMode(MUX_A2, OUTPUT); digitalWrite(MUX_A2, HIGH);
    pinMode(MUX_A3, OUTPUT); digitalWrite(MUX_A3, HIGH);
    pinMode(MUX_A4, OUTPUT); digitalWrite(MUX_A4, HIGH);

    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT,  INPUT_PULLUP);
    pinMode(ENCODER_SW,  INPUT_PULLUP);
    pinMode(MENU_BTN,    INPUT_PULLUP);
    pinMode(NEXT_CLUE_BTN, INPUT_PULLUP);
    pinMode(SELECT_BTN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, FALLING);

    initDisplays();
    logMemory("after displays init");

    Wire.begin(21, 22);
    lcd.init();
    lcd.backlight();

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        lcdPrint("LittleFS FAIL", "Check flash");
        while (1);
    }
    Serial.println("LittleFS mounted");
    logMemory("after LittleFS mount");

    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.print("FILE: "); Serial.println(file.name());
        file = root.openNextFile();
    }
    testDictionary();

    memset(grid, 0, sizeof(grid));

    // Quick diagnostic: show A on display 0 and B on display 9
    Serial.println("Testing display 0 (mux 00000)...");
    selectDisplay(0);
    delay(50);
    if (displays[0]) {
        displays[0]->begin(SSD1306_SWITCHCAPVCC);
        delay(10);
        displays[0]->clearDisplay();
        displays[0]->setRotation(3);
        displays[0]->setTextColor(WHITE);
        displays[0]->setTextSize(8);
        displays[0]->setCursor(8, 32);
        displays[0]->print('H');
        displays[0]->display();
    }
    delay(100);

    Serial.println("Testing display 1 (mux 00001)...");
    selectDisplay(1);
    delay(50);
    if (displays[1]) {
        displays[1]->begin(SSD1306_SWITCHCAPVCC);
        delay(10);
        displays[1]->clearDisplay();
        displays[1]->setRotation(3);
        displays[1]->setTextColor(WHITE);
        displays[1]->setTextSize(8);
        displays[1]->setCursor(8, 32);
        displays[1]->print('E');
        displays[1]->display();
    }
    delay(100);

    Serial.println("Testing display 2 (mux 00010)...");
    selectDisplay(2);
    delay(50);
    if (displays[2]) {
        displays[2]->begin(SSD1306_SWITCHCAPVCC);
        delay(10);
        displays[2]->clearDisplay();
        displays[2]->setRotation(3);
        displays[2]->setTextColor(WHITE);
        displays[2]->setTextSize(8);
        displays[2]->setCursor(8, 32);
        displays[2]->print('L');
        displays[2]->display();
    }
    delay(100);

    Serial.println("Testing display 3 (mux 00011)...");
    selectDisplay(3);
    delay(50);
    if (displays[3]) {
        displays[3]->begin(SSD1306_SWITCHCAPVCC);
        delay(10);
        displays[3]->clearDisplay();
        displays[3]->setRotation(3);
        displays[3]->setTextColor(WHITE);
        displays[3]->setTextSize(8);
        displays[3]->setCursor(8, 32);
        displays[3]->print('L');
        displays[3]->display();
    }
    delay(100);

    Serial.println("Testing display 4 (mux 00100)...");
    selectDisplay(4);
    delay(50);
    if (displays[4]) {
        displays[4]->begin(SSD1306_SWITCHCAPVCC);
        delay(10);
        displays[4]->clearDisplay();
        displays[4]->setRotation(3);
        displays[4]->setTextColor(WHITE);
        displays[4]->setTextSize(8);
        displays[4]->setCursor(8, 32);
        displays[4]->print('O');
        displays[4]->display();
    }
    delay(100);

    logMemory("end of setup");
    Serial.println("Setup complete");

    // Run the endurance test if enabled; then fall through to normal operation
    if (AUTOTEST_ENABLED) {
        runAutoTest();
    }

    drawMainMenu();
}

// LOOP
void loop() {
    unsigned long now = millis();

    // STATE: MAIN MENU
    if (appState == STATE_MENU) {
        if (menuChanged) {
            drawMainMenu();
            menuChanged = false;
        }

        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int delta = encoderCount - lastEncoderCount;
                lastEncoderCount = encoderCount;
                menuIndex = (menuIndex + delta + NUM_MENU_ITEMS) % NUM_MENU_ITEMS;
                menuChanged = true;
            }
        }

        if (digitalRead(SELECT_BTN) == LOW) {
            delay(50);
            if (digitalRead(SELECT_BTN) == LOW) {
                Serial.print("Menu selected: "); Serial.println(menuItems[menuIndex]);

                if (menuIndex == 0) {
                    startGeneration();
                }
                else if (menuIndex == 1) {
                    {
                        unsigned long releaseStart = millis();
                        while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                            yield();
                        }
                        delay(50);
                    }
                    openLanguageMenu(STATE_MENU);
                    return;
                }
                else if (menuIndex == 2) {
                    {
                        unsigned long releaseStart = millis();
                        while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                            yield();
                        }
                        delay(50);
                    }
                    openCheckMenu(STATE_MENU);
                    return;
                }

                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
            }
        }
        return;
    }

    // STATE: LANGUAGE MENU
    if (appState == STATE_LANGUAGE_MENU) {
        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int delta = encoderCount - lastEncoderCount;
                lastEncoderCount = encoderCount;
                languageIndex = (languageIndex + delta + NUM_LANGUAGES) % NUM_LANGUAGES;
                drawLanguageMenu();
            }
        }

        if (digitalRead(SELECT_BTN) == LOW) {
            delay(50);
            if (digitalRead(SELECT_BTN) == LOW) {
                selectedLanguage = languageIndex;
                Serial.print("Language selected: ");
                Serial.println(languageItems[selectedLanguage]);

                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }

                char conf[LCD_COLS + 1];
                snprintf(conf, sizeof(conf), "Lang set: %s", languageItems[selectedLanguage]);
                lcdPrint(conf, "Generating puzzle");
                delay(800);

                startGeneration();
                return;
            }
        }

        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(MENU_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
                encoderCount = 0;
                lastEncoderCount = 0;
                if (languageReturnState == STATE_MENU) {
                    appState = STATE_MENU;
                    menuChanged = true;
                } else if (languageReturnState == STATE_GAME_MENU) {
                    appState = STATE_GAME_MENU;
                    drawGameMenu();
                } else {
                    appState = STATE_MENU;
                    menuChanged = true;
                }
            }
        }
        return;
    }

    // STATE: ANSWER CHECK MENU
    if (appState == STATE_CHECK_MENU) {
        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int delta = encoderCount - lastEncoderCount;
                lastEncoderCount = encoderCount;
                checkMenuIndex = (checkMenuIndex + delta + NUM_CHECK_MENU_ITEMS)
                                 % NUM_CHECK_MENU_ITEMS;
                drawCheckMenu();
            }
        }

        if (digitalRead(SELECT_BTN) == LOW) {
            delay(50);
            if (digitalRead(SELECT_BTN) == LOW) {
                Serial.print("Check mode selected: ");
                Serial.println(checkMenuItems[checkMenuIndex]);
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }

                if (checkMenuIndex == 0) {
                    selectedCheckMode = CHECK_PUZZLE;
                    lcdPrint("Mode set:", "Check Puzzle");
                    delay(1000);
                    if (checkReturnState != STATE_MENU && isPuzzleFullyFilled()) {
                        int wrong = checkWholePuzzle();
                        if (wrong == 0) {
                            enterWinScreen();
                            return;
                        } else {
                            char buf[LCD_COLS + 1];
                            snprintf(buf, sizeof(buf), "%d wrong cell%s",
                                     wrong, wrong == 1 ? "" : "s");
                            lcdPrint("Checked:", buf);
                            delay(1500);
                        }
                    }
                    returnAfterCheckSelection();
                    return;
                }
                else if (checkMenuIndex == 1) {
                    selectedCheckMode = CHECK_EACH;
                    lcdPrint("Mode set:", "Check Word Cells");
                    delay(1000);
                    if (checkReturnState != STATE_MENU && currentPuzzle) {
                        int totalWrong = 0;
                        for (const auto& slot : currentPuzzle->slots) {
                            if (isSlotFullyFilled(slot)) {
                                totalWrong += checkSlotCells(slot);
                            }
                        }
                        if (totalWrong > 0) {
                            char buf[LCD_COLS + 1];
                            snprintf(buf, sizeof(buf), "%d wrong cell%s",
                                     totalWrong, totalWrong == 1 ? "" : "s");
                            lcdPrint("Checked:", buf);
                            delay(1500);
                        } else if (isPuzzleFullyFilled()) {
                            enterWinScreen();
                            return;
                        }
                    }
                    returnAfterCheckSelection();
                    return;
                }
                else if (checkMenuIndex == 2) {
                    if (checkReturnState == STATE_MENU || !currentPuzzle) {
                        lcdPrint("No active puzzle", "Generate one first");
                        delay(1500);
                        appState = STATE_MENU;
                        menuChanged = true;
                        return;
                    }
                    lcdPrint("Giving up...", "Revealing answer");
                    delay(800);
                    revealSolution();
                    lcdPrint("Solution shown", "");
                    delay(1200);
                    resumeGame();
                    return;
                }
            }
        }

        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(MENU_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
                encoderCount = 0;
                lastEncoderCount = 0;
                if (checkReturnState == STATE_MENU) {
                    appState = STATE_MENU;
                    menuChanged = true;
                } else if (checkReturnState == STATE_GAME_MENU ||
                           checkReturnState == STATE_PLAYING) {
                    resumeGame();
                } else {
                    appState = STATE_MENU;
                    menuChanged = true;
                }
            }
        }
        return;
    }

    // STATE: PLAYING
    if (appState == STATE_PLAYING) {
        if (now - lastBlinkTime >= BLINK_INTERVAL) {
            lastBlinkTime = now;
            cursorVisible = !cursorVisible;
            if (displayActive[currentDisplay]) {
                showLetter(currentDisplay, currentLetter, cursorVisible);
            }
        }

        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int pos = ((encoderCount % 26) + 26) % 26;
                currentLetter = 'A' + pos;
                lastEncoderCount = encoderCount;
                cursorVisible = true;
                lastBlinkTime = now;
                if (displayActive[currentDisplay]) {
                    showLetter(currentDisplay, currentLetter, true);
                }
                Serial.print("Count: "); Serial.print(encoderCount);
                Serial.print(" Letter: "); Serial.println(currentLetter);
            }
        }

        if (digitalRead(SELECT_BTN) == LOW) {
            delay(50);
            if (digitalRead(SELECT_BTN) == LOW) {
                grid[currentDisplay] = currentLetter;
                cellWrong[currentDisplay] = false;
                if (displayActive[currentDisplay]) {
                    showLetter(currentDisplay, currentLetter, false);
                }
                Serial.print("Confirmed '"); Serial.print(currentLetter);
                Serial.print("' on display "); Serial.println(currentDisplay);

                int editedIdx = currentDisplay;

                // Move to the next cell based on the current clue's direction.
                // For across clues, advance column-wise; for down clues, advance
                // row-wise. If we reach the end of the slot, fall through to
                // the global cell order so the "did we leave this clue?" check
                // below can fire and jump us to the next clue.
                int next = currentDisplay;
                bool advancedInSlot = false;
                if (!clueOrder.empty() && currentPuzzle) {
                    const WordSlot& curSlot =
                        currentPuzzle->slots[clueOrder[currentClueIndex]];
                    int curR = currentDisplay / 5;
                    int curC = currentDisplay % 5;

                    int nr = curR, nc = curC;
                    if (curSlot.direction == "across") {
                        nc = curC + 1;
                    } else { // down
                        nr = curR + 1;
                    }
                    // If the next cell is still inside this slot, use it
                    if (nr >= 0 && nr < GRID_SIZE && nc >= 0 && nc < GRID_SIZE &&
                        slotContainsCell(curSlot, nr, nc)) {
                        next = nr * 5 + nc;
                        advancedInSlot = true;
                    }
                }

                // If we couldn't advance inside the current slot (end of word
                // or no puzzle), fall back to linear order so the clue-change
                // logic below picks up and jumps to the next clue's start cell.
                if (!advancedInSlot) {
                    next = currentDisplay;
                    for (int attempt = 0; attempt < NUM_DISPLAYS; attempt++) {
                        next = (next + 1) % NUM_DISPLAYS;
                        if (currentPuzzle) {
                            int r = next / 5;
                            int c = next % 5;
                            if (currentPuzzle->grid[r][c] != BLACK_SQ) break;
                        } else { break; }
                    }
                }
                currentDisplay = next;

                encoderCount = 0;
                lastEncoderCount = 0;

                if (grid[currentDisplay] != 0) {
                    currentLetter = grid[currentDisplay];
                    encoderCount = currentLetter - 'A';
                    lastEncoderCount = encoderCount;
                } else {
                    currentLetter = 'A';
                }

                cursorVisible = true;
                lastBlinkTime = millis();
                if (displayActive[currentDisplay]) {
                    showLetter(currentDisplay, currentLetter, true);
                }
                Serial.print("Moved to display "); Serial.println(currentDisplay);

                // Wait for button release with a timeout so a stuck or
                // bouncing button cannot hang the UI
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }

                // Discard any encoder pulses that fired while we were busy
                // drawing the OLED and advancing the cursor. Without this,
                // stray counts cause the UI to freeze or skip letters.
                noInterrupts();
                encoderCount = 0;
                lastEncoderCount = 0;
                lastInterruptTime = millis();
                interrupts();

                if (!clueOrder.empty() && currentPuzzle) {
                    int curSlotIdx = clueOrder[currentClueIndex];
                    const WordSlot& curSlot = currentPuzzle->slots[curSlotIdx];
                    int curR = currentDisplay / 5;
                    int curC = currentDisplay % 5;

                    if (!slotContainsCell(curSlot, curR, curC)) {
                        currentClueIndex = (currentClueIndex + 1) % (int)clueOrder.size();
                        moveCursorToCurrentClue();
                        showActiveClue();
                    }
                }

                maybeCheckFilledSlots(editedIdx);
                maybeCheckFullPuzzle();
            }
        }

        if (digitalRead(NEXT_CLUE_BTN) == LOW) {
            delay(50);
            if (digitalRead(NEXT_CLUE_BTN) == LOW) {
                if (!clueOrder.empty()) {
                    currentClueIndex = (currentClueIndex + 1) % (int)clueOrder.size();
                    int slotIdx = clueOrder[currentClueIndex];
                    const WordSlot& slot = currentPuzzle->slots[slotIdx];
                    Serial.print("Next clue #"); Serial.print(currentClueIndex);
                    Serial.print(" slot="); Serial.print(slotIdx);
                    Serial.print(" dir="); Serial.print(slot.direction.c_str());
                    Serial.print(" start="); Serial.print(slot.start_row);
                    Serial.print(","); Serial.println(slot.start_col);
                    moveCursorToCurrentClue();
                    showActiveClue();
                }
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(NEXT_CLUE_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
            }
        }

        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                Serial.println("Game menu opened");
                gameMenuIndex = 0;
                encoderCount = 0;
                lastEncoderCount = 0;
                drawGameMenu();
                appState = STATE_GAME_MENU;
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(MENU_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
            }
        }
        return;
    }

    // STATE: IN-GAME MENU
    if (appState == STATE_GAME_MENU) {
        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int delta = encoderCount - lastEncoderCount;
                lastEncoderCount = encoderCount;
                gameMenuIndex = (gameMenuIndex + delta + NUM_GAME_MENU_ITEMS)
                                % NUM_GAME_MENU_ITEMS;
                drawGameMenu();
            }
        }

        if (digitalRead(SELECT_BTN) == LOW) {
            delay(50);
            if (digitalRead(SELECT_BTN) == LOW) {
                Serial.print("Game menu selected: ");
                Serial.println(gameMenuItems[gameMenuIndex]);

                if (gameMenuIndex == 0) {
                    {
                        unsigned long releaseStart = millis();
                        while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                            yield();
                        }
                        delay(50);
                    }
                    resumeGame();
                    return;
                }
                else if (gameMenuIndex == 1) {
                    {
                        unsigned long releaseStart = millis();
                        while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                            yield();
                        }
                        delay(50);
                    }
                    startGeneration();
                    return;
                }
                else if (gameMenuIndex == 2) {
                    {
                        unsigned long releaseStart = millis();
                        while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                            yield();
                        }
                        delay(50);
                    }
                    openLanguageMenu(STATE_GAME_MENU);
                    return;
                }
                else if (gameMenuIndex == 3) {
                    {
                        unsigned long releaseStart = millis();
                        while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                            yield();
                        }
                        delay(50);
                    }
                    openCheckMenu(STATE_GAME_MENU);
                    return;
                }

                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(SELECT_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
            }
        }

        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                resumeGame();
                {
                    unsigned long releaseStart = millis();
                    while (digitalRead(MENU_BTN) == LOW && millis() - releaseStart < 500) {
                        yield();
                    }
                    delay(50);
                }
            }
        }
        return;
    }

    // STATE: WIN SCREEN
    if (appState == STATE_WIN) {
        bool pressed =
            (digitalRead(SELECT_BTN) == LOW) ||
            (digitalRead(MENU_BTN) == LOW)   ||
            (digitalRead(NEXT_CLUE_BTN) == LOW);
        if (pressed) {
            delay(50);
            {
                unsigned long releaseStart = millis();
                while ((digitalRead(SELECT_BTN) == LOW ||
                        digitalRead(MENU_BTN) == LOW ||
                        digitalRead(NEXT_CLUE_BTN) == LOW) &&
                       millis() - releaseStart < 500) {
                    yield();
                }
            }
            delay(50);
            encoderCount = 0;
            lastEncoderCount = 0;
            appState = STATE_MENU;
            menuChanged = true;
        }
        return;
    }
}
