#pragma once
#include <stdint.h>
#include <time.h>

// US equity regular sessions. Eastern Time is independent of the device time zone.
// NYSE/Nasdaq published holiday and early-close calendars checked for 2026-2028.
// https://www.nyse.com/trade/hours-calendars
struct MarketSession {
  bool clockReady, tradingDay, open;
  uint32_t day;
  time_t start, end;
  int closeMinute;
};

inline int marketDaysInMonth(int y, int m) {
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  return days[m-1] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0));
}
inline int64_t marketDayNumber(int y, int m, int d) {
  int64_t days = 0;
  for (int year=1970; year<y; ++year) days += 365 + (year%4==0 && (year%100!=0 || year%400==0));
  for (int month=1; month<m; ++month) days += marketDaysInMonth(y,month);
  return days + d-1;
}
inline int marketWeekday(int y, int m, int d) { return (marketDayNumber(y,m,d)+4)%7; }
inline int marketNthWeekday(int y, int m, int weekday, int nth) {
  return 1 + (weekday-marketWeekday(y,m,1)+7)%7 + (nth-1)*7;
}
inline int marketObservedDay(int y, int m, int d) {
  int weekday=marketWeekday(y,m,d);
  return d + (weekday==6 ? -1 : weekday==0 ? 1 : 0);
}
inline void marketEaster(int y, int& month, int& day) {
  int a=y%19,b=y/100,c=y%100,d=b/4,e=b%4,f=(b+8)/25,g=(b-f+1)/3;
  int h=(19*a+b-d-g+15)%30,i=c/4,k=c%4,l=(32+2*e+2*i-h-k)%7;
  int n=(a+11*h+22*l)/451;
  month=(h+l-7*n+114)/31; day=(h+l-7*n+114)%31+1;
}
inline MarketSession marketSessionAt(time_t now) {
  MarketSession s={}; s.closeMinute=16*60;
  if (now<1700000000) return s;
  s.clockReady=true;
  struct tm utc; gmtime_r(&now,&utc);
  int y=utc.tm_year+1900;
  time_t dstStart=(time_t)(marketDayNumber(y,3,marketNthWeekday(y,3,0,2))*86400+7*3600);
  time_t dstEnd=(time_t)(marketDayNumber(y,11,marketNthWeekday(y,11,0,1))*86400+6*3600);
  int offset=(now>=dstStart && now<dstEnd) ? -4*3600 : -5*3600;
  time_t eastern=now+offset; struct tm et; gmtime_r(&eastern,&et);
  y=et.tm_year+1900; int m=et.tm_mon+1,d=et.tm_mday;
  s.day=y*10000+m*100+d;
  int thanksgiving=marketNthWeekday(y,11,4,4);
  int easterMonth,easterDay; marketEaster(y,easterMonth,easterDay);
  int goodFriday=easterDay-2;
  if (goodFriday<=0) { --easterMonth; goodFriday+=marketDaysInMonth(y,easterMonth); }
  bool holiday=(m==1 && (d==1 || (d==2 && marketWeekday(y,1,1)==0))) ||
    (m==1 && d==marketNthWeekday(y,1,1,3)) ||
    (m==2 && d==marketNthWeekday(y,2,1,3)) ||
    (m==easterMonth && d==goodFriday) ||
    (m==5 && d==marketDaysInMonth(y,5)-(marketWeekday(y,5,31)-1+7)%7) ||
    (m==6 && d==marketObservedDay(y,6,19)) ||
    (m==7 && d==marketObservedDay(y,7,4)) ||
    (m==9 && d==marketNthWeekday(y,9,1,1)) ||
    (m==11 && d==thanksgiving) ||
    (m==12 && d==marketObservedDay(y,12,25));
  s.tradingDay=et.tm_wday!=0 && et.tm_wday!=6 && !holiday;
  // Early close: Friday after Thanksgiving, weekday Christmas Eve, and July 3
  // when July 4 falls Tuesday-Friday. Friday July 3 is a holiday when July 4 is Saturday.
  int julyFourth=marketWeekday(y,7,4);
  if ((m==11 && d==thanksgiving+1) || (m==12 && d==24) ||
      (m==7 && d==3 && julyFourth>=2 && julyFourth<=5)) s.closeMinute=13*60;
  time_t midnight=(time_t)(marketDayNumber(y,m,d)*86400-offset);
  s.start=midnight+570*60; s.end=midnight+s.closeMinute*60;
  s.open=s.tradingDay && now>=s.start && now<s.end;
  return s;
}
