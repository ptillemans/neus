#pragma once

#include <stdbool.h>
#include "epoll_timerfd_utilities.h"

#ifdef OLED_SD1306
//// OLED
#include "oled.h"
#endif 

#define LSM6DSO_ID         0x6C   // register value
#define LSM6DSO_ADDRESS	   0x6A	  // I2C Address

int initI2c(void);
void closeI2c(void);

// Export to use I2C in other file
extern int i2cFd;