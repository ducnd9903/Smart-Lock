/*
LCD: SDA 18, SCL 19
Keypad: (trai sang phai) 4, 5, 6, 7, 8 ,3, 10, 1
Servo: 0
button: 9
*/

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include <driver/gpio.h>
#include "driver/i2c.h"
#include "driver/ledc.h"
      
#include "i2c_lcd.h" 

/* Khai bao Task, queue */
static void keypadReadTask(void *pvParameters);
static void authTask(void *pvParameters);
static void lcdTask(void *pvParameters);
static void servoTask(void *pvParameters);

static QueueHandle_t xQueueKeypad;
static QueueHandle_t xQueueLCD;

static TaskHandle_t xServoTaskHandle = NULL;

/* Timer cho LCD backlight */
static TimerHandle_t xBacklightTimer;
static void prvBacklightTimerCallback(TimerHandle_t xTimer);

/* Button*/
#define BUTTON_GPIO GPIO_NUM_9

/* Keypad */
#define PASSWORD "555"
#define MAX_INPUT_LEN 20

#define ROWS  4
#define COLS  4
char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
uint8_t rowPins[ROWS] = {4, 5, 6, 7};
uint8_t colPins[COLS] = {8, 3, 10, 1};

/* LCD */
typedef enum {
    LCD_TYPE_TEXT,
    LCD_TYPE_BACKLIGHT_ON,
    LCD_TYPE_BACKLIGHT_OFF
} LCD_MessageType_t;

typedef struct{
    LCD_MessageType_t type;
    char message[32];
    uint8_t row;
    uint8_t col;
    bool clrscr;
} LCD_message_t;

/* Servo */
uint32_t angle_to_duty_cycle(uint8_t angle);
#define SERVO_GPIO      GPIO_NUM_0

uint32_t angle_to_duty_cycle(uint8_t angle)
{
    if (angle > 180) angle = 180;
    // Map angle (0° -> 0.5ms, 180° -> 2.5ms)
    float pulse_width = 0.5 + (angle / 180.0) * 2.0;
    // Convert pulse width to duty cycle (12-bit resolution, 50Hz)
    uint32_t duty = (pulse_width / 20.0) * 4096;

    return duty;
}


static void prvBacklightTimerCallback(TimerHandle_t xTimer) {
    // Gửi lệnh tắt đèn nền LCD
    LCD_message_t lcd;
    lcd.type = LCD_TYPE_BACKLIGHT_OFF;
    lcd.clrscr = false;
    
    BaseType_t xStatus = xQueueSend(xQueueLCD, &lcd, pdMS_TO_TICKS(100));
    if (xStatus == pdPASS) {
        printf("Backlight OFF command sent (timeout)\n");
    } else {
        printf("Failed to send backlight OFF command\n");
    }
}


#define DEBOUNCE_TIME_MS 300
static void IRAM_ATTR button_isr_handler(void* arg)
{
    static uint32_t last_isr_time = 0;
    uint32_t current_time = xTaskGetTickCountFromISR();
    
    // debounce
    if ((current_time - last_isr_time) > pdMS_TO_TICKS(DEBOUNCE_TIME_MS))
    {
        last_isr_time = current_time;
        
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        
        // Bật lại đèn nền và reset timer
        LCD_message_t lcd;
        lcd.type = LCD_TYPE_BACKLIGHT_ON;
        lcd.clrscr = false;
        xQueueSendFromISR(xQueueLCD, &lcd, &xHigherPriorityTaskWoken);
        
        // Reset timer từ ISR
        xTimerResetFromISR(xBacklightTimer, &xHigherPriorityTaskWoken);
        
        // Thông báo cho servo task
        vTaskNotifyGiveFromISR(xServoTaskHandle, &xHigherPriorityTaskWoken);
        
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

void app_main(){
    /*Config button ISR*/
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE  
    };
    gpio_config(&io_conf);
    
    gpio_install_isr_service(0);
    
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    xQueueKeypad = xQueueCreate(10, sizeof(char));
    xQueueLCD = xQueueCreate(10, sizeof(LCD_message_t));
    
    if (xQueueKeypad != NULL && xQueueLCD != NULL) {
        
        xTaskCreate(keypadReadTask, "KeypadReaderTask", 2048, NULL, 3, NULL);
        xTaskCreate(authTask, "AuthenticationTask", 4096, NULL, 4, NULL);
        xTaskCreate(lcdTask, "LCDTask", 2048, NULL, 2, NULL);
        xTaskCreate(servoTask, "Servo_Task", 2048, NULL, 5, &xServoTaskHandle);
        
        printf("Tasks created\n");
        
        // Tạo timer cho backlight (20 giây)
        xBacklightTimer = xTimerCreate(
            "BacklightTimer",           // Tên timer
            pdMS_TO_TICKS(20000),       // 20 giây
            pdFALSE,                    // One-shot (không auto-reload)
            0,                          // Timer ID
            prvBacklightTimerCallback   // Callback function
        );
        
        if (xBacklightTimer != NULL) {
            if (xTimerStart(xBacklightTimer, 0) == pdPASS) {
                printf("Backlight timer started (20s timeout)\n");
            } else {
                printf("Backlight timer start FAILED\n");
            }
        } else {
            printf("Backlight timer creation FAILED\n");
        }
        
    } else {
        printf("Queue create failed\n");
    }
}

static void keypadReadTask(void* pvParameters){
    for (int i=0; i<ROWS; i++){
        gpio_reset_pin(rowPins[i]);
        gpio_set_direction(rowPins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(rowPins[i], 1);
    }

    for (int i = 0; i<COLS; i++){
        gpio_reset_pin(colPins[i]);
        gpio_set_direction(colPins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(colPins[i], GPIO_PULLUP_ONLY);
    }

    printf("Keypad init\n");
    char lastKey = '\0';
    while(1){
        char key = '\0';
        for(int row = 0; row<ROWS; row++){
            gpio_set_level(rowPins[row], 0);
            vTaskDelay(pdMS_TO_TICKS(1));
            for(int col = 0; col<COLS; col++){
                if(gpio_get_level(colPins[col])==0){
                    key = keyMap[row][col];
                    
                    // Bật lại đèn nền và reset timer khi có phím nhấn
                    LCD_message_t lcd_backlight;
                    lcd_backlight.type = LCD_TYPE_BACKLIGHT_ON;
                    lcd_backlight.clrscr = false;
                    xQueueSend(xQueueLCD, &lcd_backlight, 0);
                    
                    // Reset timer
                    xTimerReset(xBacklightTimer, 0);
                    
                    while(gpio_get_level(colPins[col])==0){
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
            gpio_set_level(rowPins[row], 1);
        }
        if(key!='\0' && key != lastKey){
            printf("Key pressed: %c\n", key);
            BaseType_t xStatusKeyQueueSend = xQueueSend(xQueueKeypad, &key, pdMS_TO_TICKS(100));
            if(xStatusKeyQueueSend!=pdTRUE){
                printf("Keypad send to queue failed\n");
            }
            lastKey = key;
        }
        else if(key=='\0'){
            lastKey='\0';
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void authTask(void *pvParameters){
    char input[MAX_INPUT_LEN] = "";
    char receiveKey;
    LCD_message_t lcd;

    lcd.type = LCD_TYPE_TEXT;
    strcpy(lcd.message, "Enter password:");
    lcd.row = 0;
    lcd.col = 0;
    lcd.clrscr = true;
    xQueueSend(xQueueLCD, &lcd, 0);

    printf("Auth task started\n");
    while(1){
        BaseType_t xStatusKeypadQueueReceive = xQueueReceive(xQueueKeypad, &receiveKey, portMAX_DELAY);
        if (xStatusKeypadQueueReceive == pdPASS){
            printf("Authentication received key: %c\n", receiveKey);
            if (receiveKey=='B'){
                memset(input, 0, sizeof(input));
                printf("Input cleared\n");

                lcd.type = LCD_TYPE_TEXT;
                strcpy(lcd.message, "Delete input  ");
                lcd.row = 1;
                lcd.col = 0;
                lcd.clrscr = false;
                xQueueSend(xQueueLCD, &lcd, 0);

                vTaskDelay(pdMS_TO_TICKS(1000));

                strcpy(lcd.message, "                ");
                lcd.row = 1;
                lcd.col = 0;
                lcd.clrscr = false;
                xQueueSend(xQueueLCD, &lcd, 0);
            }
            else if (receiveKey == 'A'){
                if (strcmp(input, PASSWORD) == 0){
                    printf("Password correct\n");

                    lcd.type = LCD_TYPE_TEXT;
                    strcpy(lcd.message, "Password correct!");
                    lcd.row = 0;
                    lcd.col = 0;
                    lcd.clrscr = true;
                    xQueueSend(xQueueLCD, &lcd, 0);

                    strcpy(lcd.message, "Open lock...");
                    lcd.row = 1;
                    lcd.col = 0;
                    lcd.clrscr = false;
                    xQueueSend(xQueueLCD, &lcd, 0);

                    vTaskDelay(pdMS_TO_TICKS(10));

                    xTaskNotifyGive(xServoTaskHandle);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
                else{
                    printf("Wrong password\n");

                    lcd.type = LCD_TYPE_TEXT;
                    strcpy(lcd.message, "Wrong Password!");
                    lcd.row = 0;
                    lcd.col = 0;
                    lcd.clrscr = true;
                    xQueueSend(xQueueLCD, &lcd, 0);

                    vTaskDelay(pdMS_TO_TICKS(2000));
                }

                memset(input, 0, sizeof(input));
                lcd.type = LCD_TYPE_TEXT;
                strcpy(lcd.message, "Enter password:");
                lcd.row = 0;
                lcd.col = 0;
                lcd.clrscr = true;
                xQueueSend(xQueueLCD, &lcd, 0);
            }
            else {
                int len = strlen(input);
                if(len < MAX_INPUT_LEN - 1){
                    input[len] = receiveKey;
                    input[len+1] = '\0';
                    char stars[MAX_INPUT_LEN] = "";
                    for(int i=0; i<strlen(input); i++){
                        stars[i] = '*';
                    }
                    stars[strlen(input)] = '\0';

                    lcd.type = LCD_TYPE_TEXT;
                    strcpy(lcd.message, stars);
                    lcd.row = 1;
                    lcd.col = 0;
                    lcd.clrscr = false;
                    xQueueSend(xQueueLCD, &lcd, 0);
                    printf("Current input: %s (length: %d)\n", input, (int)strlen(input));
                }
            }
        }
    }
}

static void lcdTask(void *pvParameters) {
    LCD_message_t receivedMsg;
    
    lcd_init();
    lcd_clear();
    
    // lcd_put_cursor(0, 0);
    // lcd_send_string("Smart Lock");
    // vTaskDelay(pdMS_TO_TICKS(2000));
    
    // lcd_clear();
    // lcd_put_cursor(0, 0);
    // lcd_send_string("Ready...");
    
    printf("LCD task started\n");
    
    while (1) {
        if (xQueueReceive(xQueueLCD, &receivedMsg, portMAX_DELAY) == pdPASS) {
            
            if (receivedMsg.type == LCD_TYPE_BACKLIGHT_ON) {
                lcd_backlight_on();
                printf("[LCD] Backlight ON\n");
            }
            else if (receivedMsg.type == LCD_TYPE_BACKLIGHT_OFF) {
                lcd_backlight_off();
                printf("[LCD] Backlight OFF\n");
            }

            else if (receivedMsg.type == LCD_TYPE_TEXT) {
                if (receivedMsg.clrscr) {
                    lcd_clear();
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                
                lcd_put_cursor(receivedMsg.row, receivedMsg.col);
                lcd_send_string(receivedMsg.message);
                
                printf("[LCD] Row %d, Col %d: %s\n", receivedMsg.row, receivedMsg.col, receivedMsg.message);
            }
        }
    }
}

static void servoTask(void *pvParameters){
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_12_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .channel = LEDC_CHANNEL_0,
        .duty = 0,
        .gpio_num = SERVO_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .hpoint = 0,
        .timer_sel = LEDC_TIMER_0
    };
    ledc_channel_config(&ledc_channel);

    // Đặt về vị trí 0° và giữ ổn định
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, angle_to_duty_cycle(0));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);

    printf("Servo initialized at 0 degree (locked)\n");

    while(1){
        uint32_t value = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        printf("Received %lu notify. Servo: Opening lock...\n", value);
        
        // Mở cửa: 
        for(int angle = 0 ; angle <= 120 ; angle += 10)
        {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, angle_to_duty_cycle(angle));
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        printf("Servo: Lock opened\n");

        vTaskDelay(pdMS_TO_TICKS(3000));
        
        // Đóng cửa:
        printf("Servo: Closing lock...\n");
        for(int angle = 110 ; angle >= 0 ; angle -= 10)
        {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, angle_to_duty_cycle(angle));
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
    }
}