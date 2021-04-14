/*
  support.ino - support for Tasmota

  Copyright (C) 2021  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <ext_printf.h>
//#include <SBuffer.hpp>


#define D_YEAR_MONTH_SEPARATOR "-"
#define D_MONTH_DAY_SEPARATOR "-"
#define D_DATE_TIME_SEPARATOR "T"
#define D_HOUR_MINUTE_SEPARATOR ":"
#define D_MINUTE_SECOND_SEPARATOR ":"



enum LoggingLevels { LOG_LEVEL_NONE, LOG_LEVEL_ERROR, LOG_LEVEL_INFO, LOG_LEVEL_DEBUG, LOG_LEVEL_DEBUG_MORE };
const uint16_t MAX_LOGSZ = 700;             // Max number of characters in log
const uint16_t LOGSZ = 128;                 // Max number of characters in AddLog_P log
const uint16_t LOG_BUFFER_SIZE = 4000;         // Max number of characters in logbuffer used by weblog, syslog and mqttlog
const uint8_t MAX_BACKLOG = 30;             // Max number of commands in backlog
const uint32_t MIN_BACKLOG_DELAY = 200;     // Minimal backlog delay in mSeconds
const uint8_t SENSOR_MAX_MISS = 5;          // Max number of missed sensor reads before deciding it's offline
const uint16_t INPUT_BUFFER_SIZE = 520;     // Max number of characters in serial command buffer


struct {
	uint8_t       weblog_level;              // ECE
} Settings;

struct {
	uint32_t log_buffer_pointer;              // Index in log buffer
	void *log_buffer_mutex;                   // Control access to log buffer
	uint32_t uptime;                          // Counting every second until 4294967295 = 130 year

	uint16_t seriallog_timer;                 // Timer to disable Seriallog
	uint16_t syslog_timer;                    // Timer to re-enable syslog_level
	uint16_t tele_period;                     // Tele period timer
	bool backlog_nodelay;                     // Execute all backlog commands with no delay
	bool backlog_mutex;                       // Command backlog pending
	uint8_t masterlog_level;                  // Master log level used to override set log level
	uint8_t seriallog_level;                  // Current copy of Settings.seriallog_level
	uint8_t syslog_level;                     // Current copy of Settings.syslog_level
	uint8_t templog_level;                    // Temporary log level to be used by HTTP cm and Telegram
#ifndef SUPPORT_IF_STATEMENT
	uint8_t backlog_index;                    // Command backlog index
	uint8_t backlog_pointer;                  // Command backlog pointer
	String backlog[MAX_BACKLOG];              // Command backlog buffer
#endif
	char log_buffer[LOG_BUFFER_SIZE];         // Web log buffer
	char serial_in_buffer[INPUT_BUFFER_SIZE];  // Receive buffer
	int serial_in_byte_counter;               // Index in receive buffer
} TasmotaGlobal;

struct RTC {
	uint32_t utc_time = 0;
	uint32_t local_time = 0;
	uint32_t daylight_saving_time = 0;
	uint32_t standard_time = 0;
	uint32_t midnight = 0;
	uint32_t restart_time = 0;
	uint32_t millis = 0;
	//  uint32_t last_sync = 0;
	int32_t time_timezone = 0;
	bool time_synced = false;
	bool midnight_now = false;
	bool user_time_entry = false;               // Override NTP by user setting
} Rtc;

struct TIME_T {
	uint8_t       second;
	uint8_t       minute;
	uint8_t       hour;
	uint8_t       day_of_week;               // sunday is day 1
	uint8_t       day_of_month;
	uint8_t       month;
	char          name_of_month[4];
	uint16_t      day_of_year;
	uint16_t      year;
	unsigned long days;
	unsigned long valid;
} RtcTime;



void setup()
{
}
void loop() {

}


uint32_t RtcMillis(void) {
	return (millis() - Rtc.millis) % 1000;
}


// Get span until single character in string
size_t strchrspn(const char *str1, int character)
{
	size_t ret = 0;
	char *start = (char*)str1;
	char *end = strchr(str1, character);
	if (end) ret = end - start;
	return ret;
}



bool NeedLogRefresh(uint32_t req_loglevel, uint32_t index) {

#ifdef ESP32
  // this takes the mutex, and will be release when the class is destroyed -
  // i.e. when the functon leaves  You CAN call mutex.give() to leave early.
  TasAutoMutex mutex(&TasmotaGlobal.log_buffer_mutex);
#endif  // ESP32

  // Skip initial buffer fill
  if (strlen(TasmotaGlobal.log_buffer) < LOG_BUFFER_SIZE - MAX_LOGSZ) { return false; }

  char* line;
  size_t len;
  if (!GetLog(req_loglevel, &index, &line, &len)) { return false; }
  return ((line - TasmotaGlobal.log_buffer) < LOG_BUFFER_SIZE / 4);
}

bool GetLog(uint32_t req_loglevel, uint32_t* index_p, char** entry_pp, size_t* len_p) {
  uint32_t index = *index_p;

  if (TasmotaGlobal.uptime < 3) { return false; }  // Allow time to setup correct log level
  if (!req_loglevel || (index == TasmotaGlobal.log_buffer_pointer)) { return false; }

#ifdef ESP32
  // this takes the mutex, and will be release when the class is destroyed -
  // i.e. when the functon leaves  You CAN call mutex.give() to leave early.
  TasAutoMutex mutex(&TasmotaGlobal.log_buffer_mutex);
#endif  // ESP32

  if (!index) {                            // Dump all
    index = TasmotaGlobal.log_buffer_pointer +1;
    if (index > 255) { index = 1; }
  }

  do {
    size_t len = 0;
    uint32_t loglevel = 0;
    char* entry_p = TasmotaGlobal.log_buffer;
    do {
      uint32_t cur_idx = *entry_p;
      entry_p++;
      size_t tmp = strchrspn(entry_p, '\1');
      tmp++;                               // Skip terminating '\1'
      if (cur_idx == index) {              // Found the requested entry
        loglevel = *entry_p - '0';
        entry_p++;                         // Skip loglevel
        len = tmp -1;
        break;
      }
      entry_p += tmp;
    } while (entry_p < TasmotaGlobal.log_buffer + LOG_BUFFER_SIZE && *entry_p != '\0');
    index++;
    if (index > 255) { index = 1; }        // Skip 0 as it is not allowed
    *index_p = index;
    if ((len > 0) &&
        (loglevel <= req_loglevel) &&
        (TasmotaGlobal.masterlog_level <= req_loglevel)) {
      *entry_pp = entry_p;
      *len_p = len;
      return true;
    }
    delay(0);
  } while (index != TasmotaGlobal.log_buffer_pointer);
  return false;
}

void AddLogData(uint32_t loglevel, const char* log_data) {

#ifdef ESP32
  // this takes the mutex, and will be release when the class is destroyed -
  // i.e. when the functon leaves  You CAN call mutex.give() to leave early.
  TasAutoMutex mutex(&TasmotaGlobal.log_buffer_mutex);
#endif  // ESP32

  char mxtime[14];  // "13:45:21.999 "
  snprintf_P(mxtime, sizeof(mxtime), PSTR("%02d"  D_HOUR_MINUTE_SEPARATOR  "%02d"  D_MINUTE_SECOND_SEPARATOR  "%02d.%03d "), RtcTime.hour, RtcTime.minute, RtcTime.second, RtcMillis());

  if ((loglevel <= TasmotaGlobal.seriallog_level) &&
      (TasmotaGlobal.masterlog_level <= TasmotaGlobal.seriallog_level)) {
    Serial.printf("%s%s\r\n", mxtime, log_data);
  }

  uint32_t highest_loglevel = Settings.weblog_level;
  if (TasmotaGlobal.uptime < 3) { highest_loglevel = LOG_LEVEL_DEBUG_MORE; }  // Log all before setup correct log level

  if ((loglevel <= highest_loglevel) &&    // Log only when needed
      (TasmotaGlobal.masterlog_level <= highest_loglevel)) {
    // Delimited, zero-terminated buffer of log lines.
    // Each entry has this format: [index][loglevel][log data]['\1']
    TasmotaGlobal.log_buffer_pointer &= 0xFF;
    if (!TasmotaGlobal.log_buffer_pointer) {
      TasmotaGlobal.log_buffer_pointer++;  // Index 0 is not allowed as it is the end of char string
    }
    while (TasmotaGlobal.log_buffer_pointer == TasmotaGlobal.log_buffer[0] ||  // If log already holds the next index, remove it
           strlen(TasmotaGlobal.log_buffer) + strlen(log_data) + strlen(mxtime) + 4 > LOG_BUFFER_SIZE)  // 4 = log_buffer_pointer + '\1' + '\0'
    {
      char* it = TasmotaGlobal.log_buffer;
      it++;                                // Skip log_buffer_pointer
      it += strchrspn(it, '\1');           // Skip log line
      it++;                                // Skip delimiting "\1"
      memmove(TasmotaGlobal.log_buffer, it, LOG_BUFFER_SIZE -(it-TasmotaGlobal.log_buffer));  // Move buffer forward to remove oldest log line
    }
    snprintf_P(TasmotaGlobal.log_buffer, sizeof(TasmotaGlobal.log_buffer), PSTR("%s%c%c%s%s\1"),
      TasmotaGlobal.log_buffer, TasmotaGlobal.log_buffer_pointer++, '0'+loglevel, mxtime, log_data);
    TasmotaGlobal.log_buffer_pointer &= 0xFF;
    if (!TasmotaGlobal.log_buffer_pointer) {
      TasmotaGlobal.log_buffer_pointer++;  // Index 0 is not allowed as it is the end of char string
    }
  }
}

void AddLog(uint32_t loglevel, PGM_P formatP, ...) {
  // To save stack space support logging for max text length of 128 characters
  char log_data[LOGSZ +4];

  va_list arg;
  va_start(arg, formatP);
  uint32_t len = ext_vsnprintf_P(log_data, LOGSZ +1, formatP, arg);
  va_end(arg);
  if (len > LOGSZ) { strcat(log_data, "..."); }  // Actual data is more

#ifdef DEBUG_TASMOTA_CORE
  // Profile max_len
  static uint32_t max_len = 0;
  if (len > max_len) {
    max_len = len;
    Serial.printf("PRF: AddLog %d\n", max_len);
  }
#endif

  AddLogData(loglevel, log_data);
}

void AddLog_P(uint32_t loglevel, PGM_P formatP, ...) {
  // Use more stack space to support logging for max text length of 700 characters
  char log_data[MAX_LOGSZ];

  va_list arg;
  va_start(arg, formatP);
  uint32_t len = ext_vsnprintf_P(log_data, sizeof(log_data), formatP, arg);
  va_end(arg);

  AddLogData(loglevel, log_data);
}

void AddLog_Debug(PGM_P formatP, ...)
{
  char log_data[MAX_LOGSZ];

  va_list arg;
  va_start(arg, formatP);
  uint32_t len = ext_vsnprintf_P(log_data, sizeof(log_data), formatP, arg);
  va_end(arg);

  AddLogData(LOG_LEVEL_DEBUG, log_data);
}

void AddLogBuffer(uint32_t loglevel, uint8_t *buffer, uint32_t count)
{
  char hex_char[(count * 3) + 2];
  AddLog_P(loglevel, PSTR("DMP: %s"), ToHex_P(buffer, count, hex_char, sizeof(hex_char), ' '));
}

void AddLogSerial(uint32_t loglevel)
{
  AddLogBuffer(loglevel, (uint8_t*)TasmotaGlobal.serial_in_buffer, TasmotaGlobal.serial_in_byte_counter);
}

void AddLogMissed(const char *sensor, uint32_t misses)
{
  AddLog(LOG_LEVEL_DEBUG, PSTR("SNS: %s missed %d"), sensor, SENSOR_MAX_MISS - misses);
}

void AddLogBufferSize(uint32_t loglevel, uint8_t *buffer, uint32_t count, uint32_t size) {
  char log_data[4 + (count * size * 3)];

  snprintf_P(log_data, sizeof(log_data), PSTR("DMP:"));
  for (uint32_t i = 0; i < count; i++) {
    if (1 == size) {  // uint8_t
      snprintf_P(log_data, sizeof(log_data), PSTR("%s %02X"), log_data, *(buffer));
    } else {          // uint16_t
      snprintf_P(log_data, sizeof(log_data), PSTR("%s %02X%02X"), log_data, *(buffer +1), *(buffer));
    }
    buffer += size;
  }
  AddLogData(loglevel, log_data);
}



