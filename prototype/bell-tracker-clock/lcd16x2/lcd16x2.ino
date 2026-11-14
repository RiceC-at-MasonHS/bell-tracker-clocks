
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

const char* WIFI_SSID = "MCS-Guest";
const char* WIFI_PASSWORD = "";


LiquidCrystal_I2C lcd(0x27, 16, 2);

// est+daylight saving
const char* TIME_ZONE = "EST5EDT,M3.2.0/2,M11.1.0/2";

// minutes after midnight
struct Period {
  const char* name;
  int start;
  int end;
};

// MONDAY
const Period monday[] = {
  {"Bell 1", 465, 511},
  {"Bell 2", 516, 562},
  {"Bell 3", 567, 613},
  {"Bell 4", 618, 663},
  {"Bell 5", 668, 768},
  {"Bell 6", 773, 819},
  {"Bell 7", 824, 870}
};

// TUESDAY
const Period tuesday[] = {
  {"Bell 1", 465, 535},
  {"Bell 2", 540, 610},
  {"Bell 3", 615, 685},
  {"Bell 5", 690, 795},
  {"Bell 7", 800, 870}
};

// WEDNESDAY
const Period wednesday[] = {
  {"Bell 6", 465, 535},
  {"Bell 4", 540, 610},
  {"Connect", 615, 685},
  {"Bell 5", 690, 795},
  {"Bell 7", 800, 870}
};

// THURSDAY
const Period thursday[] = {
  {"Bell 1", 465, 535},
  {"Bell 2", 540, 610},
  {"Bell 3", 615, 685},
  {"Bell 4", 690, 795},
  {"Bell 6", 800, 870}
};

// FRIDAY
const Period friday[] = {
  {"Bell 1", 465, 511},
  {"Bell 2", 516, 562},
  {"Bell 3", 567, 613},
  {"Bell 4", 618, 663},
  {"Bell 5", 668, 768},
  {"Bell 6", 773, 819},
  {"Bell 7", 824, 870}
};

// lunch
// {monday}
// {tuesday}
// {wednesday}
// {thursday}
// {friday}
const int lunchStart[5][3] = {
  {668, 703, 738},
  {690, 727, 764},
  {690, 727, 764},
  {690, 727, 764},
  {668, 703, 738}
};

const int lunchEnd[5][3] = {
  {698, 733, 768},
  {720, 757, 795},
  {720, 757, 795},
  {720, 757, 795},
  {698, 733, 768}
};

const char* lunchNames[] = {
  "A Lunch", "B Lunch", "C Lunch"
};

const Period* getSchedule(int day, int &count) {
  switch (day) {
    case 0:
      count = sizeof(monday) / sizeof(monday[0]);
      return monday;
    case 1:
      count = sizeof(tuesday) / sizeof(tuesday[0]);
      return tuesday;
    case 2:
      count = sizeof(wednesday) / sizeof(wednesday[0]);
      return wednesday;
    case 3:
      count = sizeof(thursday) / sizeof(thursday[0]);
      return thursday;
    case 4:
      count = sizeof(friday) / sizeof(friday[0]);
      return friday;
    default:
      count = 0;
      return nullptr;
  }
}

String countdown(int secondsLeft) {
  if (secondsLeft < 0) secondsLeft = 0;

  int minutes = secondsLeft / 60;
  int seconds = secondsLeft % 60;

  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%02d:%02d",
           minutes, seconds);

  return String(buffer);
}

void showLCD(String line1, String line2) {
  if (line1.length() > 16) line1 = line1.substring(0, 16);
  if (line2.length() > 16) line2 = line2.substring(0, 16);

  while (line1.length() < 16) line1 += ' ';
  while (line2.length() < 16) line2 += ' ';

  lcd.setCursor(0, 0);
  lcd.print(line1);

  lcd.setCursor(0, 1);
  lcd.print(line2);
}

String lunchTimeText(int minutes) {
  int hour = (minutes / 60) % 12;
  if (hour == 0) hour = 12;

  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%d:%02d",
           hour, minutes % 60);

  return String(buffer);
}

void setup() {
  Serial.begin(115200);

  // pins
  Wire.begin(21, 22);

  lcd.init();
  lcd.backlight();

  showLCD("MHS Bell Clock", "Connecting WiFi");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < 20000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(TIME_ZONE, "pool.ntp.org",
                 "time.nist.gov");
    showLCD("WiFi connected", "Getting time...");
  } else {
    showLCD("WiFi connection", "failed - check WiFi");
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    showLCD("WiFi disconnected", "Check connection");
    WiFi.reconnect();
    delay(2000);
    return;
  }

  struct tm now;

  if (!getLocalTime(&now, 1000)) {
    showLCD("Getting time...", "Please wait");
    delay(1000);
    return;
  }

  // Sunday=0, Monday=1, ..., Saturday=6
  int day = now.tm_wday - 1;

  if (day < 0 || day > 4) {
    showLCD("No MHS schedule", "Weekend");
    delay(1000);
    return;
  }

  int currentSeconds =
      now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec;

  int currentMinutes = currentSeconds / 60;

  
  for (int i = 0; i < 3; i++) {
    int startSeconds = lunchStart[day][i] * 60;
    int endSeconds = lunchEnd[day][i] * 60;

    if (currentSeconds >= startSeconds &&
        currentSeconds < endSeconds) {

      String remaining = countdown(endSeconds - currentSeconds);

      showLCD(String(lunchNames[i]) + " now",
              "Left " + remaining);

      delay(250);
      return;
    }
  }

  
  int periodCount = 0;
  const Period* periods = getSchedule(day, periodCount);

  for (int i = 0; i < periodCount; i++) {
    int startSeconds = periods[i].start * 60;
    int endSeconds = periods[i].end * 60;

    if (currentSeconds >= startSeconds &&
        currentSeconds < endSeconds) {

      String remaining = countdown(endSeconds - currentSeconds);

      // first row
      String line1 = String(periods[i].name) + " " + remaining;

      // second row
      String line2 = "Next lunch: none";

      for (int j = 0; j < 3; j++) {
        if (lunchStart[day][j] > currentMinutes) {
          line2 = String(lunchNames[j]) + " " +
                  lunchTimeText(lunchStart[day][j]);
          break;
        }
      }

      showLCD(line1, line2);
      delay(250);
      return;
    }
  }

  
  for (int i = 0; i < 3; i++) {
    if (lunchStart[day][i] > currentMinutes) {
      String line2 = String(lunchNames[i]) + " " +
                     lunchTimeText(lunchStart[day][i]);

      if (currentMinutes < 465) {
        showLCD("Before school", line2);
      } else {
        showLCD("Between bells", line2);
      }

      delay(250);
      return;
    }
  }

  
  if (currentMinutes < 465) {
    showLCD("Before school", "Starts 7:45 AM");
  } else {
    showLCD("School finished", "Have a good day!");
  }

  delay(250);
}
