/**
 * @file app_tasks.h
 * @brief Declaració de les tasques de FreeRTOS de l'aplicació.
 */

#ifndef APP_TASKS_H
#define APP_TASKS_H

void rtc_task(void *pv);
void alarm_task(void *pv);
void ui_task(void *pv);

#endif // APP_TASKS_H
