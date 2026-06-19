/**
 * @file app_tasks.h
 * @brief Declaració de les tasques de FreeRTOS de l'aplicació.
 */

#ifndef APP_TASKS_H
#define APP_TASKS_H

void task_rtc(void *pv);
void task_alarm(void *pv);
void task_ui(void *pv);

#endif // APP_TASKS_H
