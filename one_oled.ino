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
#define ENCODER_SW   32
#define MENU_BTN     33
#define NEXT_CLUE_BTN 19

#define NUM_DISPLAYS 25
#define BLINK_INTERVAL 500
#define MUX_OFFSET 0

// LCD geometry
#define LCD_COLS 20
#define LCD_ROWS 4

// =============================================================================
// STATE MACHINE
// =============================================================================
enum AppState {
    STATE_MENU,
    STATE_LANGUAGE_MENU,
    STATE_GENERATING,
    STATE_PLAYING,
    STATE_GAME_MENU
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
const char* languageItems[] = { "English", "Spanish" };
const char* languageCodes[] = { "english", "spanish" };
const int NUM_LANGUAGES = 2;
int languageIndex = 0;           // currently-highlighted item in language menu
int selectedLanguage = 0;        // actively-used language (0=english, 1=spanish)

// Remember which menu opened the language submenu so we can return
AppState languageReturnState = STATE_MENU;

// Puzzle
std::optional<PuzzleGrid> currentPuzzle;
WordDB word_db;
bool dictLoaded = false;

// Clue ordering
std::vector<int> clueOrder;
int currentClueIndex = 0;

// =============================================================================
// HARDWARE
// =============================================================================
LiquidCrystal_I2C lcd(0x27, LCD_COLS, LCD_ROWS);

Adafruit_SSD1306* displays[NUM_DISPLAYS];
bool displayActive[NUM_DISPLAYS];

// List which displays are physically connected
// Update this array as you wire up more displays
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

// =============================================================================
// ISR
// =============================================================================
void IRAM_ATTR encoderISR() {
    unsigned long now = millis();
    if (now - lastInterruptTime < 100) return;
    lastInterruptTime = now;
    if (digitalRead(ENCODER_DT) != digitalRead(ENCODER_CLK))
        encoderCount++;
    else
        encoderCount--;
}

// =============================================================================
// MUX + OLED HELPERS
// =============================================================================
void selectDisplay(uint8_t index) {
    uint8_t ch = index + MUX_OFFSET;
    digitalWrite(MUX_A0, LOW); digitalWrite(MUX_A1, LOW);
    digitalWrite(MUX_A2, LOW); digitalWrite(MUX_A3, LOW);
    digitalWrite(MUX_A4, LOW);
    delay(5);
    digitalWrite(MUX_A0, (ch >> 0) & 1);
    digitalWrite(MUX_A1, (ch >> 1) & 1);
    digitalWrite(MUX_A2, (ch >> 2) & 1);
    digitalWrite(MUX_A3, (ch >> 3) & 1);
    digitalWrite(MUX_A4, (ch >> 4) & 1);
    delay(10);
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
    d->setCursor(0, 10);
    d->print(letter);
    if (showCursor) {
        d->setTextSize(2);
        d->setCursor(0, 90);
        d->print("_");
    }
    d->display();
}

void clearOLED(uint8_t index) {
    if (index >= NUM_DISPLAYS || !displayActive[index]) return;

    selectDisplay(index);
    Adafruit_SSD1306* d = displays[index];
    d->clearDisplay();
    d->display();
}

// =============================================================================
// LCD HELPERS (20x4)
// =============================================================================

// Print a single line at a given row, clearing the rest of that row
void lcdPrintRow(int row, const char* text) {
    lcd.setCursor(0, row);
    int len = strlen(text);
    if (len > LCD_COLS) len = LCD_COLS;
    for (int i = 0; i < len; i++) lcd.print(text[i]);
    for (int i = len; i < LCD_COLS; i++) lcd.print(' ');
}

// Simple 2-line message (line2 optional) — uses top two rows, clears bottom rows
void lcdPrint(const char* line0, const char* line1 = "") {
    lcd.clear();
    lcdPrintRow(0, line0);
    lcdPrintRow(1, line1);
}

// Generic scrolling list renderer for 20x4 LCD.
// Shows a title on row 0 and up to 3 items on rows 1..3, with '>' next to the selected item.
// Generic scrolling list renderer for 20x4 LCD.
// Shows up to 4 items on rows 0..3, with '>' next to the selected item.
void drawList(const char* title, const char* const* items, int count, int selected) {
    lcd.clear();

    // All 4 rows available for items (title ignored)
    const int visibleRows = LCD_ROWS;  // 4

    // Scroll window so selected item is visible
    int startIdx = 0;
    if (count > visibleRows) {
        if (selected >= visibleRows) {
            startIdx = selected - (visibleRows - 1);
        }
        if (startIdx + visibleRows > count) {
            startIdx = count - visibleRows;
        }
        if (startIdx < 0) startIdx = 0;
    }

    for (int row = 0; row < visibleRows; row++) {
        int itemIdx = startIdx + row;
        if (itemIdx >= count) {
            lcdPrintRow(row, "");
            continue;
        }
        char buf[LCD_COLS + 1];
        snprintf(buf, sizeof(buf), "%c%s",
                 (itemIdx == selected) ? '>' : ' ',
                 items[itemIdx]);
        lcdPrintRow(row, buf);
    }
}
void drawMainMenu() {
    drawList("main", menuItems, NUM_MENU_ITEMS, menuIndex);
}

void drawGameMenu() {
    drawList("gam", gameMenuItems, NUM_GAME_MENU_ITEMS, gameMenuIndex);
}

void drawLanguageMenu() {
    // Build a title that shows the currently-active language
    char title[LCD_COLS + 1];
    snprintf(title, sizeof(title), "Language (now:%s)", languageItems[selectedLanguage]);
    // Truncate cleanly if needed (should fit in 20)
    drawList(title, languageItems, NUM_LANGUAGES, languageIndex);
}

// =============================================================================
// CLUE DISPLAY (uses rows 0-3 on the 20x4 LCD)
// =============================================================================
void buildClueOrder() {
    clueOrder.clear();
    if (!currentPuzzle) return;

    for (int i = 0; i < (int)currentPuzzle->slots.size(); i++) {
        if (currentPuzzle->slots[i].direction == "across")
            clueOrder.push_back(i);
    }
    for (int i = 0; i < (int)currentPuzzle->slots.size(); i++) {
        if (currentPuzzle->slots[i].direction == "down")
            clueOrder.push_back(i);
    }
    currentClueIndex = 0;
}

int getClueNumber(const PuzzleGrid& puzzle, int slotIndex) {
    const WordSlot& target = puzzle.slots[slotIndex];
    int num = 0;
    for (int i = 0; i <= slotIndex; i++) {
        if (puzzle.slots[i].direction == target.direction)
            num++;
    }
    return num;
}

// Word-wrap a string across up to `maxRows` rows of width LCD_COLS.
// Writes into rows[0..maxRows-1], each null-terminated and padded to LCD_COLS.
void wrapText(const char* text, char rows[][LCD_COLS + 1], int maxRows) {
    for (int i = 0; i < maxRows; i++) rows[i][0] = '\0';

    int len = strlen(text);
    int pos = 0;
    int rowIdx = 0;

    while (pos < len && rowIdx < maxRows) {
        // How many chars remain
        int remaining = len - pos;
        if (remaining <= LCD_COLS) {
            // Fits in one row
            strncpy(rows[rowIdx], text + pos, LCD_COLS);
            rows[rowIdx][LCD_COLS] = '\0';
            pos = len;
            rowIdx++;
            break;
        }
        // Find last space within LCD_COLS window
        int splitAt = LCD_COLS;
        for (int i = LCD_COLS; i > 0; i--) {
            if (text[pos + i] == ' ') { splitAt = i; break; }
        }
        // If no space found, hard-split
        if (splitAt == 0 || splitAt == LCD_COLS) splitAt = LCD_COLS;

        strncpy(rows[rowIdx], text + pos, splitAt);
        rows[rowIdx][splitAt] = '\0';
        pos += splitAt;
        // Skip leading space on next line
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

    // Row 0: header like "1A  (3 of 8)"
    char header[LCD_COLS + 1];
    snprintf(header, sizeof(header), "%d%c  (%d of %d)",
             num, dirChar,
             clueIdx + 1, (int)clueOrder.size());

    // Rows 1..3: the clue text, wrapped
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

// =============================================================================
// LITTLEFS DICTIONARY TEST
// =============================================================================
void testDictionary() {
    File f = LittleFS.open("/dictionaries/spanish_3.txt", "r");
    if (!f) {
        Serial.println("TEST FAIL: could not open spanish_3.txt");
        lcdPrint("Dict test FAIL", "File not found");
        return;
    }
    String line = f.readStringUntil('\n');
    f.close();
    Serial.print("TEST PASS: first line = ");
    Serial.println(line);
    lcdPrint("Dict OK:", line.c_str());
}

// =============================================================================
// DICTIONARY LOAD — words only, no clues
// =============================================================================
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

    Serial.print("Heap before dict: "); Serial.println(ESP.getFreeHeap());

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
        Serial.print(" words from length "); Serial.println(len);
        Serial.print("  Heap: "); Serial.println(ESP.getFreeHeap());
    }

    dictLoaded = true;
    Serial.println("Dictionary loaded successfully");
    return true;
}

// =============================================================================
// CLUE LOOKUP
// =============================================================================
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

// =============================================================================
// PUZZLE GENERATION
// =============================================================================
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

    if (dictLoaded) freeDictionary();

    if (!loadDictionary(langCode)) {
        lcdPrint("Dict not found", langCode);
        delay(2000);
        appState = STATE_MENU;
        menuChanged = true;
        return;
    }

    Serial.print("Free heap before generate: ");
    Serial.println(ESP.getFreeHeap());

    currentPuzzle = generate_puzzle(word_db, langCode, 20);

    if (!currentPuzzle) {
        lcdPrint("Gen failed :(", "Try again");
        delay(2000);
        appState = STATE_MENU;
        menuChanged = true;
        return;
    }

    freeDictionary();
    Serial.println("Dictionary freed");
    Serial.print("Free heap after generate: ");
    Serial.println(ESP.getFreeHeap());

    fillCluesFromFile(*currentPuzzle, langCode);
    buildClueOrder();

    // Re-initialize displays after LittleFS operations
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
    encoderCount     = 0;
    lastEncoderCount = 0;
    cursorVisible    = true;
    lastBlinkTime    = millis();

    // Make sure OLED reset is high
    digitalWrite(OLED_RESET, HIGH);
    delay(10);

    // Display puzzle on OLEDs — letters and white screens for black squares
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
                    // White screen for black squares
                    displays[idx]->fillScreen(WHITE);
                    displays[idx]->display();
                    delay(10);
                    Serial.print("Display "); Serial.print(idx); Serial.println(" -> WHITE");
                } else if (cell != EMPTY_SQ) {
                    // Show the answer letter
                    displays[idx]->clearDisplay();
                    displays[idx]->setRotation(3);
                    displays[idx]->setTextColor(WHITE);
                    displays[idx]->setTextSize(8);
                    displays[idx]->setCursor(0, 10);
                    displays[idx]->print(cell);
                    displays[idx]->display();
                    Serial.print("Display "); Serial.print(idx);
                    Serial.print(" -> "); Serial.println(cell);
                }
            }
        }
    }

    showActiveClue();
    moveCursorToCurrentClue();

    print_puzzle(*currentPuzzle);

    appState = STATE_PLAYING;
    Serial.println("Puzzle ready");
}

// =============================================================================
// RESUME HELPER
// =============================================================================
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

// =============================================================================
// LANGUAGE MENU — opens from either main or game menu
// =============================================================================
void openLanguageMenu(AppState returnTo) {
    languageReturnState = returnTo;
    languageIndex = selectedLanguage;   // start highlighted on the active language
    encoderCount = 0;
    lastEncoderCount = 0;
    appState = STATE_LANGUAGE_MENU;
    drawLanguageMenu();
}

// =============================================================================
// DISPLAY INIT — only initializes physically connected displays
// =============================================================================
void initDisplays() {
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, HIGH); delay(10);
    digitalWrite(OLED_RESET, LOW);  delay(10);
    digitalWrite(OLED_RESET, HIGH); delay(10);

    for (int i = 0; i < NUM_DISPLAYS; i++) {
        displays[i] = nullptr;
        displayActive[i] = false;
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
    for (int i = 0; i < NUM_DISPLAYS; i++) {
        if (displayActive[i]) count++;
    }
    Serial.print("Active displays: "); Serial.println(count);
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    if (psramInit()) {
        Serial.println("PSRAM initialized");
    } else {
        Serial.println("PSRAM not available");
    }

    Serial.print("PSRAM size: "); Serial.println(ESP.getPsramSize());
    Serial.print("Free PSRAM: "); Serial.println(ESP.getFreePsram());
    Serial.print("Free heap:  "); Serial.println(ESP.getFreeHeap());

    pinMode(MUX_A0, OUTPUT); digitalWrite(MUX_A0, LOW);
    pinMode(MUX_A1, OUTPUT); digitalWrite(MUX_A1, LOW);
    pinMode(MUX_A2, OUTPUT); digitalWrite(MUX_A2, LOW);
    pinMode(MUX_A3, OUTPUT); digitalWrite(MUX_A3, LOW);
    pinMode(MUX_A4, OUTPUT); digitalWrite(MUX_A4, LOW);

    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT,  INPUT_PULLUP);
    pinMode(ENCODER_SW,  INPUT_PULLUP);
    pinMode(MENU_BTN,    INPUT_PULLUP);
    pinMode(NEXT_CLUE_BTN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, FALLING);

    initDisplays();

    Wire.begin(21, 22);
    lcd.init();
    lcd.backlight();

    if (!LittleFS.begin()) {
        Serial.println("LittleFS mount failed");
        lcdPrint("LittleFS FAIL", "Check flash");
        while (1);
    }
    Serial.println("LittleFS mounted");

    File root = LittleFS.open("/");
    File file = root.openNextFile();
    while (file) {
        Serial.print("FILE: ");
        Serial.println(file.name());
        file = root.openNextFile();
    }
    testDictionary();

    memset(grid, 0, sizeof(grid));

    // Test display 0 — mux address 00000
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
        displays[0]->setCursor(0, 10);
        displays[0]->print('A');
        displays[0]->display();
        Serial.println("Display 0 should show A");
    }
    delay(3000);

    // Test display 9 — mux address 01001
    Serial.println("Testing display 9 (mux 01001)...");
    selectDisplay(9);
    delay(50);
    if (displays[9]) {
        displays[9]->begin(SSD1306_SWITCHCAPVCC);
        delay(10);
        displays[9]->clearDisplay();
        displays[9]->setRotation(3);
        displays[9]->setTextColor(WHITE);
        displays[9]->setTextSize(8);
        displays[9]->setCursor(0, 10);
        displays[9]->print('B');
        displays[9]->display();
        Serial.println("Display 9 should show B");
    }
    delay(3000);

    drawMainMenu();
    Serial.println("Setup complete");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
    unsigned long now = millis();

    // =========================================================================
    // STATE: MAIN MENU
    // =========================================================================
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

        if (digitalRead(ENCODER_SW) == LOW) {
            delay(50);
            if (digitalRead(ENCODER_SW) == LOW) {
                Serial.print("Menu selected: "); Serial.println(menuItems[menuIndex]);

                if (menuIndex == 0) {
                    startGeneration();
                }
                else if (menuIndex == 1) {
                    // Open language selector (return to main menu afterward)
                    while (digitalRead(ENCODER_SW) == LOW);
                    delay(50);
                    openLanguageMenu(STATE_MENU);
                    return;
                }
                else if (menuIndex == 2) {
                    lcdPrint("Answer Check", "Coming soon...");
                    delay(2000);
                    menuChanged = true;
                }

                while (digitalRead(ENCODER_SW) == LOW);
                delay(50);
            }
        }
        return;
    }

    // =========================================================================
    // STATE: LANGUAGE MENU
    // =========================================================================
    if (appState == STATE_LANGUAGE_MENU) {
        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int delta = encoderCount - lastEncoderCount;
                lastEncoderCount = encoderCount;
                languageIndex = (languageIndex + delta + NUM_LANGUAGES) % NUM_LANGUAGES;
                drawLanguageMenu();
            }
        }

        // Encoder press = select this language and immediately regenerate
        if (digitalRead(ENCODER_SW) == LOW) {
            delay(50);
            if (digitalRead(ENCODER_SW) == LOW) {
                selectedLanguage = languageIndex;
                Serial.print("Language selected: ");
                Serial.println(languageItems[selectedLanguage]);

                while (digitalRead(ENCODER_SW) == LOW);
                delay(50);

                // Brief confirmation, then regenerate a puzzle in the new language
                char conf[LCD_COLS + 1];
                snprintf(conf, sizeof(conf), "Lang set: %s", languageItems[selectedLanguage]);
                lcdPrint(conf, "Generating puzzle");
                delay(800);

                startGeneration();
                return;
            }
        }

        // Menu button = cancel, go back without changing language
        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                Serial.println("Language menu cancelled");
                while (digitalRead(MENU_BTN) == LOW);
                delay(50);

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

    // =========================================================================
    // STATE: PLAYING
    // =========================================================================
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

        // Encoder button — confirm letter
        if (digitalRead(ENCODER_SW) == LOW) {
            delay(50);
            if (digitalRead(ENCODER_SW) == LOW) {
                grid[currentDisplay] = currentLetter;
                if (displayActive[currentDisplay]) {
                    showLetter(currentDisplay, currentLetter, false);
                }
                Serial.print("Confirmed '"); Serial.print(currentLetter);
                Serial.print("' on display "); Serial.println(currentDisplay);

                // Move to next non-black-square cell
                int next = currentDisplay;
                for (int attempt = 0; attempt < NUM_DISPLAYS; attempt++) {
                    next = (next + 1) % NUM_DISPLAYS;
                    if (currentPuzzle) {
                        int r = next / 5;
                        int c = next % 5;
                        if (currentPuzzle->grid[r][c] != BLACK_SQ)
                            break;
                    } else {
                        break;
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

                while (digitalRead(ENCODER_SW) == LOW);
                delay(50);
            }
        }

        // Next clue button
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
                while (digitalRead(NEXT_CLUE_BTN) == LOW);
                delay(50);
            }
        }

        // Menu button
        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                Serial.println("Game menu opened");
                gameMenuIndex = 0;
                encoderCount = 0;
                lastEncoderCount = 0;
                drawGameMenu();
                appState = STATE_GAME_MENU;
                while (digitalRead(MENU_BTN) == LOW);
                delay(50);
            }
        }
        return;
    }

    // =========================================================================
    // STATE: IN-GAME MENU
    // =========================================================================
    if (appState == STATE_GAME_MENU) {
        if (encoderCount != lastEncoderCount) {
            if (now - lastInterruptTime > 50) {
                int delta = encoderCount - lastEncoderCount;
                lastEncoderCount = encoderCount;
                gameMenuIndex = (gameMenuIndex + delta + NUM_GAME_MENU_ITEMS) % NUM_GAME_MENU_ITEMS;
                drawGameMenu();
            }
        }

        if (digitalRead(ENCODER_SW) == LOW) {
            delay(50);
            if (digitalRead(ENCODER_SW) == LOW) {
                Serial.print("Game menu selected: "); Serial.println(gameMenuItems[gameMenuIndex]);

                if (gameMenuIndex == 0) {
                    while (digitalRead(ENCODER_SW) == LOW);
                    delay(50);
                    resumeGame();
                    return;
                }
                else if (gameMenuIndex == 1) {
                    while (digitalRead(ENCODER_SW) == LOW);
                    delay(50);
                    startGeneration();
                    return;
                }
                else if (gameMenuIndex == 2) {
                    // Language selector — will regenerate on selection,
                    // or return to game menu if cancelled
                    while (digitalRead(ENCODER_SW) == LOW);
                    delay(50);
                    openLanguageMenu(STATE_GAME_MENU);
                    return;
                }
                else if (gameMenuIndex == 3) {
                    lcdPrint("Answer Check", "Coming soon...");
                    delay(2000);
                    drawGameMenu();
                }

                while (digitalRead(ENCODER_SW) == LOW);
                delay(50);
            }
        }

        if (digitalRead(MENU_BTN) == LOW) {
            delay(50);
            if (digitalRead(MENU_BTN) == LOW) {
                resumeGame();
                while (digitalRead(MENU_BTN) == LOW);
                delay(50);
            }
        }
        return;
    }
}
