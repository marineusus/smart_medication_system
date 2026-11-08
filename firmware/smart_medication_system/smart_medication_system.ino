//!!!!!!!!!!!!!테스트 시  811번째 줄!!!!!!!!!!! 현재시간 원하면 또 따로 한번 설정해줘야함

#include <RTClib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <vector>
#include <SPI.h>
#include <SD.h>
#include <math.h>

#define SENSOR_PIN 4
#define BUZZER_PIN 17

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define SD_CS 5
#define SD_SCK 48
#define SD_MISO 47
#define SD_MOSI 38

// ================= 알고리즘 설정값 =================
const int MIN_BASELINE_RECORDS = 5;      // Baseline 생성에 필요한 최소 복용 기록 수
const int MIN_RESULT_RECORDS = 5;        // 확률 계산을 시작하기 위한 최소 SUCCESS/FAILURE 수
const int SUCCESS_RANGE_MINUTES = 10;    // Baseline 기준 성공 인정 범위: ±10분
const int ALERT_BEFORE_MINUTES = 1;      // Baseline 1분 전에 알림
const double ALERT_THRESHOLD = 0.70;     // 성공 확률 70% 미만이면 알림 대상
const double DECAY_LAMBDA = 0.0495;      // ln(2) / 14 ≈ 0.0495 → 반감기 약 14일

const int MORNING_START = 6;
const int MORNING_END = 10;
const int NOON_START = 11;
const int NOON_END = 15;
const int EVENING_START = 17;
const int EVENING_END = 21;

const unsigned long SENSOR_STABLE_MS = 200;

// 날짜별 복용 성공/실패 기록
struct ResultRecord {
  String date;   // 예: "2026-09-01"
  bool success;  // true = SUCCESS, false = FAILURE
};

std::vector<ResultRecord> morningResults;
std::vector<ResultRecord> noonResults;
std::vector<ResultRecord> eveningResults;

// 시:분을 하루 기준 총 분으로 변환
int toMinutes(int hour, int minute) {
  return hour * 60 + minute;
}

// ================= RTC =================
class RTCModule {
private:
  RTC_DS3231 rtc;

public:
  void init() {
    Wire.begin(11, 12); // SDA=11, SCL=12

    if(!rtc.begin()) {
      Serial.println("RTC 연결 실패!");
      while(1);
    }
  }

  DateTime getTime() {
    return rtc.now();
  }

  // 테스트용 시간 조작
  void setFakeTime(int year, int month, int day, int hour, int minute, int second) {
    rtc.adjust(DateTime(year, month, day, hour, minute, second));
    Serial.printf("RTC 시간 변경: %04d-%02d-%02d %02d:%02d:%02d\n",
                  year, month, day, hour, minute, second);
  }
};

// ================= 약통 센서 =================
class SensorModule {
public:
  void init() {
    pinMode(SENSOR_PIN, INPUT_PULLUP);
  }

  bool isClosed() {
    int sensorValue = digitalRead(SENSOR_PIN);
    return (sensorValue == LOW); // LOW = 닫힘
  }
};

// ================= OLED =================
class OLEDModule {
private:
  Adafruit_SSD1306 display;

public:
  OLEDModule() : display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1) {}

  void init() {
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
      Serial.println("OLED 초기화 실패!");
      while(1);
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
  }

  void show(DateTime now, bool closed) {
    display.clearDisplay();
    display.setCursor(0, 8);

    // 날짜
    display.print(now.year());
    display.print("-");
    display.print(now.month());
    display.print("-");
    display.println(now.day());

    // 시간
    display.setCursor(0, 25);
    display.print(now.hour());
    display.print(":");

    if(now.minute() < 10)
      display.print("0");

    display.print(now.minute());
    display.print(":");

    if(now.second() < 10)
      display.print("0");

    display.println(now.second());

    // 약통 상태
    display.setCursor(0, 50);
    if(closed) {
      display.println("Medicine CLOSED");
    } else {
      display.println("Medicine OPEN");
    }

    display.display();
  }

  // 자정에 계산된 Baseline을 OLED에 5초간 표시
  void showBaseline(int mH, int mM, int nH, int nM, int eH, int eM) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 20);
    display.println("BASELINE");

    display.setCursor(0, 32);
    display.print("Morning  ");
    if(mH < 10) display.print("0");
    display.print(mH);
    display.print(":");
    if(mM < 10) display.print("0");
    display.println(mM);

    display.setCursor(0, 44);
    display.print("Noon     ");
    if(nH < 10) display.print("0");
    display.print(nH);
    display.print(":");
    if(nM < 10) display.print("0");
    display.println(nM);

    display.setCursor(0, 56);
    display.print("Evening  ");
    if(eH < 10) display.print("0");
    display.print(eH);
    display.print(":");
    if(eM < 10) display.print("0");
    display.println(eM);

    display.display();
  }
};

// ================= 부저 =================
class BuzzerModule {
private:
  int pin;

public:
  BuzzerModule(int p) {
    pin = p;
  }

  void init() {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }

  void alert(int freq = 1000, int duration = 300, int repeat = 3) {
    for(int i = 0; i < repeat; i++) {
      tone(pin, freq, duration);
      delay(duration + 100);
    }
    noTone(pin);
  }
};

// ================= 복용 시간 기록 =================
// 같은 날짜의 같은 시간대는 학습용 기록을 1개만 사용
bool hasMealRecordForDate(String label, DateTime date) {
  File file = SD.open("/" + label + ".txt");
  if(!file) return false;

  char target[11];
  sprintf(target, "%04d-%02d-%02d", date.year(), date.month(), date.day());

  while(file.available()) {
    String line = file.readStringUntil('\n');

    if(line.startsWith(target)) {
      file.close();
      return true;
    }
  }

  file.close();
  return false;
}

// 실제 복용 시간 SD 카드에 저장
void saveMealRecord(String label, DateTime now) {
  if(hasMealRecordForDate(label, now)) {
    Serial.printf("[%s] 오늘 복용시간은 이미 기록되어 있습니다.\n", label.c_str());
    return;
  }

  File file = SD.open("/" + label + ".txt", FILE_APPEND);

  if(file) {
    file.printf("%04d-%02d-%02d %02d:%02d\n",
                now.year(), now.month(), now.day(),
                now.hour(), now.minute());

    file.close();

    Serial.printf("[%s] 복용시간 저장: %04d-%02d-%02d %02d:%02d\n",
                  label.c_str(),
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute());
  } else {
    Serial.println("SD 파일 열기 실패");
  }
}

// 날짜 YYYY-MM-DD 형태의 문자열로 변환
String formatDate(DateTime date) {
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", date.year(), date.month(), date.day());
  return String(buf);
}

// 해당 날짜의 결과 확인 / 1 = SUCCESS, -1 = FAILURE, 0 = 기록 없음
int getResultStatus(String label, DateTime date) {
  File file = SD.open("/" + label + "_result.txt");
  if(!file) return 0;

  String target = formatDate(date);
  int status = 0;

  while(file.available()) {
    String line = file.readStringUntil('\n');

    if(line.startsWith(target)) {
      if(line.indexOf("SUCCESS") > 0) {
        file.close();
        return 1;
      }

      if(line.indexOf("FAILURE") > 0) {
        status = -1;
      }
    }
  }

  file.close();
  return status;
}

// 특정 날짜의 결과를 제거한 뒤 파일을 다시 저장
void removeResultForDate(String label, DateTime date, String statusToRemove) {
  String path = "/" + label + "_result.txt";
  String tempPath = "/" + label + "_result_tmp.txt";

  File src = SD.open(path);
  if(!src) return;

  SD.remove(tempPath.c_str());
  // 이전 임시파일을 지우고 새 파일 생성
  File tmp = SD.open(tempPath, FILE_WRITE);

  if(!tmp) {
    src.close();
    return;
  }

  String target = formatDate(date);

  while(src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if(line.length() == 0) continue;

    // target 날짜 + 제거 대상 상태인 줄은 건너뜀
    if(line.startsWith(target) && line.indexOf(statusToRemove) > 0) {
      continue;
    }
    tmp.println(line);
  }
  src.close();
  tmp.close();

  SD.remove(path);
  SD.rename(tempPath.c_str(), path.c_str());
}

// SUCCESS / FAILURE 결과 저장
// 실제로 새로운 결과를 저장하거나 FAILURE→SUCCESS로 갱신했으면 true 반환
bool saveResult(String label, DateTime date, bool success) {
  int oldResult = getResultStatus(label, date);

  // 이미 SUCCESS가 있으면 같은 날짜 결과는 더 저장하지 않음
  if(oldResult == 1) {
    if(success) {
      Serial.printf("[%s] 이미 같은 날짜 SUCCESS 기록이 있습니다.\n", label.c_str());
    } else {
      Serial.printf("[%s] 이미 SUCCESS 기록이 있어 FAILURE는 무시합니다.\n", label.c_str());
    }
    return false;
  }

  // FAILURE가 이미 있는 경우
  if(oldResult == -1) {
    if(!success) {
      Serial.printf("[%s] 이미 같은 날짜 FAILURE 기록이 있습니다.\n", label.c_str());
      return false;
    }

    // 시간 착오 등으로 먼저 FAILURE가 생겼더라도 이후 ±10분 범위에서 정상 복용하면 FAILURE를 지우고 SUCCESS로 갱신
    Serial.printf("[%s] FAILURE를 제거하고 SUCCESS로 갱신합니다.\n", label.c_str());
    removeResultForDate(label, date, "FAILURE");
  }

  File file = SD.open("/" + label + "_result.txt", FILE_APPEND);

  if(file) {
    file.printf("%04d-%02d-%02d %s\n",
                date.year(), date.month(), date.day(),
                success ? "SUCCESS" : "FAILURE");

    file.close();

    Serial.printf("[%s] 결과 저장: %04d-%02d-%02d %s\n",
                  label.c_str(),
                  date.year(), date.month(), date.day(),
                  success ? "SUCCESS" : "FAILURE");

    return true;
  }

  Serial.println("SD 파일 열기 실패");
  return false;
}


// ================= Baseline 관리 =================
// 초기 복용기록의 평균시간을 계산하고 SD 카드에 기준시간을 저장/불러오기
class BaselineManager {
private:
  struct Baseline {
    int hour;
    int minute;
    bool valid;
  };

  Baseline morning, noon, evening;
  DateTime baselineCreatedDate;   // baseline을 처음 만든 날짜
  bool baselineCreated = false;   // baseline 생성 여부

public:
  BaselineManager() {
    morning = {-1, -1, false};
    noon = {-1, -1, false};
    evening = {-1, -1, false};
    baselineCreated = false;
  }

  // baseline 생성일 반환
  DateTime getBaselineCreatedDate() { return baselineCreatedDate; }
  bool isBaselineCreated() { return baselineCreated; }

  // 아침 기준시간
  int getMorningHour() { return morning.hour; }
  int getMorningMinute() { return morning.minute; }
  bool isMorningReady() { return morning.valid; }

  // 점심
  int getNoonHour() { return noon.hour; }
  int getNoonMinute() { return noon.minute; }
  bool isNoonReady() { return noon.valid; }

  // 저녁
  int getEveningHour() { return evening.hour; }
  int getEveningMinute() { return evening.minute; }
  bool isEveningReady() { return evening.valid; }

  // SD 카드에 저장된 baseline 불러오기
  void loadFromSD() {
    File file = SD.open("/baseline.txt");
    if(!file) {
      Serial.println("baseline.txt 파일 없음 → 아직 평균 기준시간이 계산되지 않았습니다.");
      return;
    }

    while(file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();

      if(line.startsWith("Created=")) {
        String d = line.substring(8); // "YYYY-MM-DD"
        int y = d.substring(0, 4).toInt();
        int mo = d.substring(5, 7).toInt();
        int dd = d.substring(8, 10).toInt();

        baselineCreatedDate = DateTime(y, mo, dd, 0, 0, 0);
        baselineCreated = true;
        Serial.printf("Baseline 생성일 불러옴: %04d-%02d-%02d\n", y, mo, dd);
        continue;
      }

      int sep = line.indexOf('=');
      int colon = line.indexOf(':');

      if(sep > 0 && colon > 0) {
        String meal = line.substring(0, sep);
        int hour = line.substring(sep + 1, colon).toInt();
        int minute = line.substring(colon + 1).toInt();

        if(meal == "Morning") {
          morning = {hour, minute, true};
        } else if(meal == "Noon") {
          noon = {hour, minute, true};
        } else if(meal == "Evening") {
          evening = {hour, minute, true};
        }

        Serial.printf("[%s] 불러온 기준시간: %02d:%02d\n", meal.c_str(), hour, minute);
      }
    }

    file.close();
  }

  // 계산된 아침/점심/저녁 Baseline을 메모리, SD 카드에 저장
  void saveAllBaselines(int mH, int mM, int nH, int nM, int eH, int eM, DateTime now) {
    if(!baselineCreated) {
      baselineCreatedDate = now;
      baselineCreated = true;
    }

    morning = {mH, mM, true};
    noon = {nH, nM, true};
    evening = {eH, eM, true};

    // Baseline은 최신 값 한 세트만 유지
    SD.remove("/baseline.txt");
    File file = SD.open("/baseline.txt", FILE_WRITE);

    if(file) {
      file.printf("Morning=%02d:%02d\n", mH, mM);
      file.printf("Noon=%02d:%02d\n", nH, nM);
      file.printf("Evening=%02d:%02d\n", eH, eM);
      file.printf("Created=%04d-%02d-%02d\n",
                  baselineCreatedDate.year(),
                  baselineCreatedDate.month(),
                  baselineCreatedDate.day());
      file.close();
      Serial.println("Baseline 세트 저장 완료");
    }
  }

  // 문자열로 아침/점심/저녁 기준시간 가져오기
  int getHour(String meal) {
    if(meal == "Morning" && morning.valid) return morning.hour;
    if(meal == "Noon" && noon.valid) return noon.hour;
    if(meal == "Evening" && evening.valid) return evening.hour;
    return -1;
  }

  int getMinute(String meal) {
    if(meal == "Morning" && morning.valid) return morning.minute;
    if(meal == "Noon" && noon.valid) return noon.minute;
    if(meal == "Evening" && evening.valid) return evening.minute;
    return -1;
  }

  // 저장된 복용시간의 평균을 계산
  // 같은 날짜의 기록이 여러 개 있어도 하루 1개만 계산
  // 서로 다른 날짜 기준 최소 5개의 기록이 있어야 Baseline 후보로 사용
  bool analyzeAverageTime(String label, int &avgHour, int &avgMinute) {
    File file = SD.open("/" + label + ".txt");
    if(!file) {
      Serial.printf("[%s] 파일 없음\n", label.c_str());
      return false;
    }

    int count = 0;
    int recentTimes[MIN_BASELINE_RECORDS];
    std::vector<String> usedDates;

    while(file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();

      if(line.length() < 16) continue;

      String date = line.substring(0, 10);
      bool alreadyCounted = false;

      for(int i = 0; i < usedDates.size(); i++) {
        if(usedDates[i] == date) {
          alreadyCounted = true;
          break;
        }
      }

      if(alreadyCounted) continue;

      int hour = line.substring(11, 13).toInt();
      int minute = line.substring(14, 16).toInt();

      recentTimes[count % MIN_BASELINE_RECORDS] = toMinutes(hour, minute);
      count++;
      usedDates.push_back(date);
    }

    file.close();

    if(count < MIN_BASELINE_RECORDS) {
      Serial.printf("[%s] 서로 다른 날짜의 기록이 %d개라 Baseline 계산을 보류합니다.\n",
                    label.c_str(), count);
      return false;
    }

    int sum = 0;
    for(int i = 0; i < MIN_BASELINE_RECORDS; i++) {
      sum += recentTimes[i];
    }

    int avg = sum / MIN_BASELINE_RECORDS;
    avgHour = avg / 60;
    avgMinute = avg % 60;

    Serial.printf("[%s] 최근 %d일 기록 평균 Baseline: %02d:%02d\n",
                  label.c_str(), MIN_BASELINE_RECORDS, avgHour, avgMinute);

    return true;
  }
};

// ================= 복용 결과 판정 =================
// 약통을 연 시간이 기준시간에 맞는지 판단하고 SUCCESS/FAILURE 저장
class ResultManager {
private:
  BaselineManager* bm;   // 기준시간 정보를 사용하기 위한 포인터

public:
  ResultManager(BaselineManager* manager) {
    this->bm = manager;
  }

  // 약통을 연 시간이 baseline ±10분 안이면 SUCCESS
  bool checkEvent(String label, DateTime now) {
    int baselineMinutes = toMinutes(bm->getHour(label), bm->getMinute(label));
    if(baselineMinutes < 0) return false;

    int nowMinutes = toMinutes(now.hour(), now.minute());
    bool success =
        (abs(nowMinutes - baselineMinutes) <= SUCCESS_RANGE_MINUTES);

    bool resultSaved = saveResult(label, now, success);

    // 정상 복용이 새로 기록된 경우 실제 복용시간도 학습 데이터로 저장
    // 같은 날짜에는 1개만 저장됨
    if(success && resultSaved) {
      saveMealRecord(label, now);
    }

    return success;
  }

  // 하루가 끝난 뒤 기록이 없으면 FAILURE 저장
  void checkDailyFailure(DateTime targetDate) {
    if(!bm->isBaselineCreated()) {
      Serial.println("Baseline 아직 없음 → 실패 평가 보류");
      return;
    }

    DateTime created = bm->getBaselineCreatedDate();

    // 날짜만 비교하기 위해 자정 기준으로 맞춤
    DateTime createdDay(created.year(), created.month(), created.day());
    DateTime targetDay(targetDate.year(), targetDate.month(), targetDate.day());

    // baseline 생성일 이전 또는 생성 당일은 실패 평가에서 제외
    if(targetDay.unixtime() <= createdDay.unixtime()) {
      Serial.println("Baseline 생성 이전(또는 생성 당일) → 실패 평가 제외");
      return;
    }

    // saveResult()에서 기존 SUCCESS/FAILURE 여부를 확인
    saveResult("Morning", targetDate, false);
    saveResult("Noon", targetDate, false);
    saveResult("Evening", targetDate, false);
  }

  bool hasSuccessRecord(String label, DateTime date) {
    return getResultStatus(label, date) == 1;
  }
};

// SD 카드의 SUCCESS / FAILURE 기록을 메모리로 불러오기
void loadResultsFromSD(const char* filename, std::vector<ResultRecord>& results) {
  File file = SD.open(filename);
  if(!file) {
    Serial.printf("%s 파일 없음\n", filename);
    return;
  }

  int count = 0;

  while(file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if(line.length() == 0) continue;

    int spaceIdx = line.indexOf(' ');
    if(spaceIdx > 0) {
      String date = line.substring(0, spaceIdx);
      String status = line.substring(spaceIdx+1);

      ResultRecord rec;
      rec.date = date;
      rec.success = (status == "SUCCESS");
      results.push_back(rec);
      count++;
    }
  }

  file.close();
  Serial.printf("[%s] 기록 %d개 불러옴\n", filename, count);
}

// 아침/점심/저녁 결과 파일을 메모리에 다시 불러오기
void reloadResultData() {
  morningResults.clear();
  noonResults.clear();
  eveningResults.clear();

  loadResultsFromSD("/Morning_result.txt", morningResults);
  loadResultsFromSD("/Noon_result.txt", noonResults);
  loadResultsFromSD("/Evening_result.txt", eveningResults);
}

// ================= 복용 성공 확률 계산 =================
class ProbabilityCalculator { // 최근 기록에 더 큰 가중치를 주어 성공 확률 계산
public:
  int successCount;
  int failureCount;

  double weightedSuccess;
  double weightedFailure;

  double alpha;
  double beta;
  double mean;   // 최종 성공 확률 추정치
  double lambda; // 오래된 기록의 가중치를 줄이는 값

  ProbabilityCalculator(double decayLambda = DECAY_LAMBDA) { // 반감기 약 14일
    lambda = decayLambda;
    reset();
  }

  void reset() {
    successCount = 0;
    failureCount = 0;
    weightedSuccess = 0.0;
    weightedFailure = 0.0;
    alpha = 1.0;
    beta = 1.0;
    mean = 0.5; // 초기 성공 확률
  }

  // 날짜 문자열을 DateTime으로 변환
  static DateTime parseDate(const String& dateStr) {
    int y = dateStr.substring(0, 4).toInt();
    int mo = dateStr.substring(5, 7).toInt();
    int d = dateStr.substring(8, 10).toInt();
    return DateTime(y, mo, d, 0, 0, 0);
  }

  // 오늘과 기록 날짜의 차이를 이용해 최근 기록일수록 큰 가중치 적용
  // 최소 5개 이상의 결과가 있어야 계산
  bool calculate(const std::vector<ResultRecord>& results, DateTime now) {
    if(results.size() < MIN_RESULT_RECORDS) {
      Serial.println("SUCCESS/FAILURE 기록이 5개 미만이라 확률 계산을 보류합니다.");
      return false;
    }

    reset();
    DateTime today(now.year(), now.month(), now.day());

    for(const auto& rec : results) {
      DateTime recDate = parseDate(rec.date);
      long daysAgo = (long)((today.unixtime() - recDate.unixtime()) / 86400L);
      if(daysAgo < 0) daysAgo = 0; // 미래 날짜가 들어오면 오늘 기록으로 처리

      double weight = exp(-lambda * (double)daysAgo); // 최근 기록일수록 큰 가중치

      if(rec.success) {
        successCount++;
        weightedSuccess += weight;
      } else {
        failureCount++;
        weightedFailure += weight;
      }
    }

    // 가중치가 적용된 성공/실패 값을 이용해 성공 확률 계산
    alpha = weightedSuccess + 1.0;
    beta = weightedFailure + 1.0;
    mean = alpha / (alpha + beta);

    return true;
  }

  void printStats(const char* label) {
    Serial.printf("%s → 성공=%d, 실패=%d, 기대값=%.3f\n",
                  label, successCount, failureCount, mean);
  }
};


// ================= 객체 및 상태 변수 =================
RTCModule rtcModule;
SensorModule sensorModule;
OLEDModule oledModule;
BuzzerModule buzzer(BUZZER_PIN);

ProbabilityCalculator morningCalc(DECAY_LAMBDA);
ProbabilityCalculator noonCalc(DECAY_LAMBDA);
ProbabilityCalculator eveningCalc(DECAY_LAMBDA);

bool morningAlertNeeded = false;
bool noonAlertNeeded = false;
bool eveningAlertNeeded = false;

// 이전 센서 상태 저장
bool previousOpen = false;

unsigned long openStartMillis = 0;
bool openEventHandled = false;
// 센서 200ms 이상 열려있을 때만 실제 개방 처리

int lastSecond = -1;
bool lastClosed = false;
bool analyzedToday = false;     // Baseline 계산을 하루 한 번만 실행
bool statsPrintedToday = false; // 확률 계산을 하루 한 번만 실행

int lastFailureCheckDay = -1;

BaselineManager baselineManager;
ResultManager resultManager(&baselineManager);

// 복용 성공 확률이 기준값보다 낮으면 알림이 필요하다고 표시
void evaluateAlertFlags(DateTime now) {
  if(morningCalc.calculate(morningResults, now)) {
    morningCalc.printStats("Morning");
    morningAlertNeeded = (morningCalc.mean < ALERT_THRESHOLD);
  }
  if(noonCalc.calculate(noonResults, now)) {
    noonCalc.printStats("Noon");
    noonAlertNeeded = (noonCalc.mean < ALERT_THRESHOLD);
  }
  if(eveningCalc.calculate(eveningResults, now)) {
    eveningCalc.printStats("Evening");
    eveningAlertNeeded = (eveningCalc.mean < ALERT_THRESHOLD);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  while(!Serial) {}

  // RTC, 센서, OLED, 부저 초기화
  rtcModule.init();
  sensorModule.init();
  oledModule.init();
  buzzer.init();

  // 테스트할 때 주석 해제해 사용
  rtcModule.setFakeTime(2026, 9, 9, 23, 59, 50);
 
  // SD 카드 초기화
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if(!SD.begin(SD_CS)) {
    Serial.println("SD 카드 연결 실패!");
  } else {
    Serial.println("SD 카드 연결 성공!");
  }

  // 저장된 baseline 불러오기
  baselineManager.loadFromSD();

  // 자정에 기기가 꺼져 있었을 수 있으므로 부팅할 때 전날 기록도 확인
  DateTime setupNow = rtcModule.getTime();
  DateTime setupYesterday = setupNow - TimeSpan(1, 0, 0, 0);
  resultManager.checkDailyFailure(setupYesterday);
  lastFailureCheckDay = setupNow.day();

  // 기존 성공/실패 결과 불러오고 현재 확률 계산
  reloadResultData();
  evaluateAlertFlags(setupNow);

  // 오늘 이미 성공한 시간대는 재부팅 후에도 알림을 다시 울리지 않음
  if(resultManager.hasSuccessRecord("Morning", setupNow)) {
    morningAlertNeeded = false;
  }

  if(resultManager.hasSuccessRecord("Noon", setupNow)) {
    noonAlertNeeded = false;
  }

  if(resultManager.hasSuccessRecord("Evening", setupNow)) {
    eveningAlertNeeded = false;
  }
}

// ================= LOOP =================
void loop() {
  DateTime now = rtcModule.getTime();
  bool closed = sensorModule.isClosed();
  bool open = !closed;
  unsigned long currentMillis = millis();

  // ---------- OLED 화면 표시 ----------
  // 시간이 바뀌거나 약통 OPEN/CLOSED 상태가 바뀔 때만 화면 갱신
  if(now.second() != lastSecond || closed != lastClosed) {
    oledModule.show(now, closed);
    lastSecond = now.second();
    lastClosed = closed;
  }

  // ---------- 약통 열림 감지 ----------
  // 약통이 200ms 이상 열려 있을 때만 실제 개방으로 판단
  if(open && !previousOpen) {
    openStartMillis = currentMillis;
  }

  if(!open) {
    openStartMillis = 0;
    openEventHandled = false;
  }

  bool stableOpenEvent =
      open &&
      !openEventHandled &&
      (currentMillis - openStartMillis >= SENSOR_STABLE_MS);

  if(stableOpenEvent) {
    openEventHandled = true; // 순간적인 센서 튐은 무시

    int eventMinutes = toMinutes(now.hour(), now.minute());
    bool eventProcessed = false;

    // ---------- 복용시간 판정 ----------
    // baseline이 있으면 먼저 기준시간 ±10분 안인지 확인
    if(baselineManager.isMorningReady()) {
      int morningBase = toMinutes(baselineManager.getMorningHour(), baselineManager.getMorningMinute());
      if(abs(eventMinutes - morningBase) <= SUCCESS_RANGE_MINUTES) {
        if(resultManager.checkEvent("Morning", now))
          morningAlertNeeded = false;
        eventProcessed = true;
      }
    }

    if(!eventProcessed && baselineManager.isNoonReady()) {
      int noonBase = toMinutes(baselineManager.getNoonHour(), baselineManager.getNoonMinute());
      if(abs(eventMinutes - noonBase) <= SUCCESS_RANGE_MINUTES) {
        if(resultManager.checkEvent("Noon", now))
          noonAlertNeeded = false;
        eventProcessed = true;
      }
    }

    if(!eventProcessed && baselineManager.isEveningReady()) {
      int eveningBase = toMinutes(baselineManager.getEveningHour(), baselineManager.getEveningMinute());
      if(abs(eventMinutes - eveningBase) <= SUCCESS_RANGE_MINUTES) {
        if(resultManager.checkEvent("Evening", now))
          eveningAlertNeeded = false;
        eventProcessed = true;
      }
    }

    // 기준시간에 해당하지 않으면 기존 아침/점심/저녁 시간대로 처리
    if(!eventProcessed) {
      if(now.hour() >= MORNING_START && now.hour() <= MORNING_END) {
        if(!baselineManager.isMorningReady()) {
          // baseline 생성 전에는 복용시간을 학습용 기록으로 저장
          saveMealRecord("Morning", now);
        } else {
          resultManager.checkEvent("Morning", now);
        }
        eventProcessed = true;
      }

      else if(now.hour() >= NOON_START && now.hour() <= NOON_END) {
        if(!baselineManager.isNoonReady()) {
          saveMealRecord("Noon", now);
        } else {
          resultManager.checkEvent("Noon", now);
        }
        eventProcessed = true;
      }

      else if(now.hour() >= EVENING_START && now.hour() <= EVENING_END) {
        if(!baselineManager.isEveningReady()) {
          saveMealRecord("Evening", now);
        } else {
          resultManager.checkEvent("Evening", now);
        }
        eventProcessed = true;
      }
    }

    if(eventProcessed)
      Serial.println("기록 추가됨");
  }

  previousOpen = open;

  // ---------- 하루 단위 처리 ----------
  // 00:00 - 누적 복용시간을 이용해 Baseline 생성 또는 갱신
  if(now.hour() == 0 && now.minute() == 0 && !analyzedToday) {
    int mH = -1, mM = -1;
    int nH = -1, nM = -1;
    int eH = -1, eM = -1;

    bool morningOk = baselineManager.analyzeAverageTime("Morning", mH, mM);
    bool noonOk = baselineManager.analyzeAverageTime("Noon", nH, nM);
    bool eveningOk = baselineManager.analyzeAverageTime("Evening", eH, eM);

    // 세 시간대 모두 최소 기록 수를 만족하면 Baseline 저장, 갱신
    if(morningOk && noonOk && eveningOk) {
      baselineManager.saveAllBaselines(mH, mM, nH, nM, eH, eM, now);

      oledModule.showBaseline(mH, mM, nH, nM, eH, eM);
      delay(5000);
    } else if(!baselineManager.isBaselineCreated()) {
      Serial.println("데이터 부족 → Baseline 생성 보류");
    }

    analyzedToday = true;
  }

  // 00:01 - 다음 날 baseline 확인을 위해 플래그 초기화
  if(now.hour() == 0 && now.minute() == 1 && analyzedToday) {
    analyzedToday = false;
  }

  // 00:02 - 전날 복용 기록이 없으면 FAILURE 저장
  if(now.hour() == 0 && now.minute() == 2 && lastFailureCheckDay != now.day()) {
    DateTime yesterday = now - TimeSpan(1, 0, 0, 0);
    resultManager.checkDailyFailure(yesterday);
    lastFailureCheckDay = now.day();
  }

  // 00:03 - 새 날짜의 알림 상태 초기화
  if(now.hour() == 0 && now.minute() == 3) {
    morningAlertNeeded = false;
    noonAlertNeeded = false;
    eveningAlertNeeded = false;
  }

  // 00:04 - 최신 결과를 다시 읽고 성공 확률 계산
  if(now.hour() == 0 && now.minute() == 4 && !statsPrintedToday) {
    reloadResultData();
    evaluateAlertFlags(now);
    statsPrintedToday = true;
  }

  // 00:05 - 다음 날 확률 계산을 위해 플래그 초기화
  if(now.hour() == 0 && now.minute() == 5) {
    statsPrintedToday = false;
  }

  // ---------- 부저 알림 ----------
  // 성공 확률이 낮은 시간대는 baseline 1분 전에 부저를 울림
  int nowMinutes = toMinutes(now.hour(), now.minute());

  // 아침
  int morningBaselineMinutes = toMinutes(baselineManager.getMorningHour(), baselineManager.getMorningMinute());
  if(baselineManager.isMorningReady() &&
     nowMinutes == morningBaselineMinutes - ALERT_BEFORE_MINUTES &&
     morningAlertNeeded &&
     !open) {

    buzzer.alert();
    Serial.println("Morning 확률 낮음 → 부저 울림!");
    morningAlertNeeded = false;
  }

  // 점심
  int noonBaselineMinutes = toMinutes(baselineManager.getNoonHour(), baselineManager.getNoonMinute());
  if(baselineManager.isNoonReady() &&
     nowMinutes == noonBaselineMinutes - ALERT_BEFORE_MINUTES &&
     noonAlertNeeded &&
     !open) {

    buzzer.alert();
    Serial.println("Noon 확률 낮음 → 부저 울림!");
    noonAlertNeeded = false;
  }

  // 저녁
  int eveningBaselineMinutes = toMinutes(baselineManager.getEveningHour(), baselineManager.getEveningMinute());
  if(baselineManager.isEveningReady() &&
     nowMinutes == eveningBaselineMinutes - ALERT_BEFORE_MINUTES &&
     eveningAlertNeeded &&
     !open) {

    buzzer.alert();
    Serial.println("Evening 확률 낮음 → 부저 울림!");
    eveningAlertNeeded = false;
  }
  delay(20);
}