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
#define MUX_A4  16

#define ENCODER_CLK  25
#define ENCODER_DT   26
#define ENCODER_SW   32
#define MENU_BTN     33
#define NEXT_CLUE_BTN 19

#define NUM_DISPLAYS 25
#define BLINK_INTERVAL 500
#define MUX_OFFSET 0

// =============================================================================
// STATE MACHINE
// =============================================================================
enum AppState {
    STATE_MENU,
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
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Array of 25 displays — all share the same SPI pins, mux selects CS
Adafruit_SSD1306* displays[NUM_DISPLAYS];
bool displayActive[NUM_DISPLAYS];  // tracks which displays initialized OK

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
    d->clearDisplay();
    d->setRotation(1);
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
// LCD HELPERS
// =============================================================================
void lcdPrint(const char* line0, const char* line1 = "") {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(line0);
    lcd.setCursor(0, 1); lcd.print(line1);
}

void drawMainMenu() {
    lcd.clear();
    char row0[17];
    snprintf(row0, sizeof(row0), ">%s", menuItems[menuIndex]);
    lcd.setCursor(0, 0);
    lcd.print(row0);
    int next = menuIndex + 1;
    if (next < NUM_MENU_ITEMS) {
        char row1[17];
        snprintf(row1, sizeof(row1), " %s", menuItems[next]);
        lcd.setCursor(0, 1);
        lcd.print(row1);
    }
}

void drawGameMenu() {
    lcd.clear();
    char row0[17];
    snprintf(row0, sizeof(row0), ">%s", gameMenuItems[gameMenuIndex]);
    lcd.setCursor(0, 0);
    lcd.print(row0);
    int next = gameMenuIndex + 1;
    if (next < NUM_GAME_MENU_ITEMS) {
        char row1[17];
        snprintf(row1, sizeof(row1), " %s", gameMenuItems[next]);
        lcd.setCursor(0, 1);
        lcd.print(row1);
    }
}

// =============================================================================
// CLUE DISPLAY
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

void showNumberedClue(int clueIdx) {
    if (!currentPuzzle || clueIdx < 0 || clueIdx >= (int)clueOrder.size()) return;

    int slotIdx = clueOrder[clueIdx];
    const WordSlot& slot = currentPuzzle->slots[slotIdx];
    int num = getClueNumber(*currentPuzzle, slotIdx);
    char dirChar = (slot.direction == "across") ? 'A' : 'D';

    char prefix[6];
    snprintf(prefix, sizeof(prefix), "%d%c:", num, dirChar);

    char full[33];
    snprintf(full, sizeof(full), "%s%s", prefix, slot.clue.c_str());

    int len = strlen(full);
    lcd.clear();
    if (len <= 16) {
        lcd.setCursor(0, 0);
        lcd.print(full);
    } else {
        int splitPos = 16;
        for (int i = 15; i > 0; i--) {
            if (full[i] == ' ') { splitPos = i; break; }
        }
        lcd.setCursor(0, 0);
        for (int i = 0; i < splitPos; i++) lcd.print(full[i]);

        lcd.setCursor(0, 1);
        const char* rest = full + splitPos;
        if (*rest == ' ') rest++;
        int printed = 0;
        while (*rest && printed < 16) { lcd.print(*rest++); printed++; }
    }
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
    File f = LittleFS.open("/dictionaries/english_3.txt", "r");
    if (!f) {
        Serial.println("TEST FAIL: could not open english_3.txt");
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
    lcdPrint("Loading dict...", "");

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
    lcdPrint("Generating...", "Please wait");

    if (dictLoaded) freeDictionary();

    if (!loadDictionary("english")) {
        lcdPrint("Dict not found", "");
        delay(2000);
        appState = STATE_MENU;
        menuChanged = true;
        return;
    }

    Serial.print("Free heap before generate: ");
    Serial.println(ESP.getFreeHeap());

    currentPuzzle = generate_puzzle(word_db, "english", 20);

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

    fillCluesFromFile(*currentPuzzle, "english");
    buildClueOrder();

    currentDisplay = 0;
    currentLetter  = 'A';
    memset(grid, 0, sizeof(grid));
    encoderCount     = 0;
    lastEncoderCount = 0;
    cursorVisible    = true;
    lastBlinkTime    = millis();

    // Clear all active displays
    for (int i = 0; i < NUM_DISPLAYS; i++) {
        if (displayActive[i]) clearOLED(i);
    }

    // Show black squares on displays that have them
    if (currentPuzzle) {
        for (int r = 0; r < GRID_SIZE; r++) {
            for (int c = 0; c < GRID_SIZE; c++) {
                int idx = r * 5 + c;
                if (currentPuzzle->grid[r][c] == BLACK_SQ && displayActive[idx]) {
                    // Leave black square displays blank/off
                    clearOLED(idx);
                }
            }
        }
    }

    moveCursorToCurrentClue();
    showActiveClue();

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
// DISPLAY INIT
// =============================================================================
void initDisplays() {
    pinMode(OLED_RESET, OUTPUT);
    digitalWrite(OLED_RESET, HIGH); delay(10);
    digitalWrite(OLED_RESET, LOW);  delay(10);
    digitalWrite(OLED_RESET, HIGH); delay(10);

    for (int i = 0; i < NUM_DISPLAYS; i++) {
        displays[i] = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT,
            OLED_MOSI, OLED_CLK, OLED_DC, -1, -1);
        displayActive[i] = false;

        selectDisplay(i);
        if (displays[i]->begin(SSD1306_SWITCHCAPVCC)) {
            displays[i]->clearDisplay();
            displays[i]->display();
            displayActive[i] = true;
            Serial.print("Display "); Serial.print(i); Serial.println(" OK");
        } else {
            Serial.print("Display "); Serial.print(i); Serial.println(" not found");
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

    // Initialize all displays
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
                    lcdPrint("Language", "Coming soon...");
                    delay(2000);
                    menuChanged = true;
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
    // STATE: PLAYING
    // =========================================================================
    if (appState == STATE_PLAYING) {
        // Blink cursor only on active displays
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
                    resumeGame();
                }
                else if (gameMenuIndex == 1) {
                    startGeneration();
                }
                else if (gameMenuIndex == 2) {
                    lcdPrint("Language", "Coming soon...");
                    delay(2000);
                    drawGameMenu();
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