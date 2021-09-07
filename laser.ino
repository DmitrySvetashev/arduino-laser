/*  
 *  программа управления мощностью лазера в зависимости от положения виртуальной оси и корректирующего коэффициента, зависящего от скорости движения портала по осям XY.
 *  программа работает на микроконтроллере arduino, подключенном к плате управления станком, работающей под MACH 3. 
 *  является переработанной версией идеи пользователя serg1958, взятой отсюда https://www.cnc-club.ru/forum/viewtopic.php?t=9337. за что serg1958 большое спасибо.
 *  может быть использована без ограничений. все вопросы по адресу d.sve@mail.ru. 2021г.
 */ 

#include <avr/EEPROM.h>
#include <LiquidCrystal.h>
#include <avr/io.h>
#include <avr/interrupt.h>
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);     // объявляем к каким пинам подключен LCD


#define STEP_LASER_PIN  3                 // входной сигнал (Step) изменения мошности лазера
#define DIR_LASER_PIN   2                 // входной сигнал (Dir) направления изменения мошности лазера
#define MIN_LASER_POWER 0                 // минимальная мощность лазера, соответствующая 0% загрузке
#define MAX_LASER_POWER 255               // максимальная мощность лазера, соответствующая 100% загрузке
#define LASER_PIN       11                // ШИМ выход управления мощностью лазера
#define KEY_PIN         A0                // вход для кнопок
#define STEPX_PIN       A1                // входной сигнал (Step)по оси X
#define STEPY_PIN       A2                // входной сигнал (Step)по оси Y
#define BUTTON_NONE     0                 // коды кнопок - ничего не нажато
#define BUTTON_RIGHT    1                 // коды кнопок - кнопка вправо
#define BUTTON_UP       2                 // коды кнопок - кнопка вверх
#define BUTTON_DOWN     3                 // коды кнопок - кнопка вниз
#define BUTTON_LEFT     4                 // коды кнопок - кнопка влево
#define BUTTON_SELECT   5                 // коды кнопок - кнопка выбор
int PAGE = 3;                             // номер экрана при запуске
int STP = 5;                              // шаг изменения параметров
int KEY;                                  // код кнопки
struct SETUP_VALUE {                      // объявление структуры данных для сохранени параметров в EPROM
  int OFFSET;                             // минимальная мощность лазера (баланс белого) 0-255
  int MAX_LOAD;                           // максимальная мощность лазера 0-255
  int XY_VEL_CORR;                        // коэффициент коррекции мощности лазера от скорости движения по осям XY 0-100%
};
SETUP_VALUE SetupVal;
volatile int LASER_AXIS_POS;              // положение виртуальной оси, связанной с мощностью лазера
float LASER_POWER;                        // текущая мощность лазера
float POWER_CORR;                         // корректирующий коэффициент
volatile unsigned long X_POWER;           // Значение АЦП, считанное с оси X
volatile unsigned long Y_POWER;           // Значение АЦП, считанное с оси Y


// Подпрограмма получение значения нажатой кнопки
int getPressedButton()
{
  int buttonValue = analogRead(KEY_PIN);  // считываем значения с аналогового входа
  if (buttonValue > 800) {
    return BUTTON_NONE;                   // если не было нажатия - выйти
  }
  delay(70);
  int buttonValue1 = analogRead(KEY_PIN); // считываем значения с аналогового входа повторно через паузу
  if (buttonValue == buttonValue1)        // было нажатие и не дребезг контактов
  {
    delay(150);
    if (buttonValue1 < 100) return BUTTON_RIGHT;
    else if (buttonValue < 200) return BUTTON_UP;
    else if (buttonValue < 400) return BUTTON_DOWN;
    else if (buttonValue < 600) return BUTTON_LEFT;
    else if (buttonValue < 800) return BUTTON_SELECT;
    return BUTTON_NONE;
  }
}


//  Подпрограмма вывода в LCD
void OUTLCD(bool clr)
{
  if (clr) lcd.clear();
  switch (PAGE)
  {
    case 0 : lcd.setCursor(0, 0);
      lcd.print("SETUP");
      lcd.setCursor(0, 1);
      lcd.print("Min power       ");
      lcd.setCursor(13, 1);
      lcd.print(SetupVal.OFFSET);
      break;
    case 1 : lcd.setCursor(0, 0);
      lcd.print("SETUP");
      lcd.setCursor(0, 1);
      lcd.print("Max power      ");
      lcd.setCursor(13, 1);
      lcd.print(SetupVal.MAX_LOAD);
      break;
    case 2 : lcd.setCursor(0, 0);
      lcd.print("SETUP");
      lcd.setCursor(0, 1);
      lcd.print("XY vel corr%   ");
      lcd.setCursor(13, 1);
      lcd.print(SetupVal.XY_VEL_CORR);
      break;
    case 3 : lcd.setCursor(0, 0);
      lcd.print("STATS  axis     ");
      lcd.setCursor(12, 0);
      lcd.print(-LASER_AXIS_POS);
      lcd.setCursor(0, 1);
      lcd.print("c      power    ");
      lcd.setCursor(2, 1);
      lcd.print(round(POWER_CORR));
      lcd.setCursor(13, 1);
      lcd.print(round(LASER_POWER));
      break;
  }
}


//П/П изменения параметра
void CHNGPAR (int stp)
{
  switch (PAGE)
  {
    case 0 : SetupVal.OFFSET += stp;
      SetupVal.OFFSET = constrain(SetupVal.OFFSET, MIN_LASER_POWER, SetupVal.MAX_LOAD);
      break;
    case 1 : SetupVal.MAX_LOAD += stp;
      SetupVal.MAX_LOAD = constrain(SetupVal.MAX_LOAD, SetupVal.OFFSET, MAX_LASER_POWER);
      break;
    case 2 : SetupVal.XY_VEL_CORR +=stp;
      SetupVal.XY_VEL_CORR = constrain(SetupVal.XY_VEL_CORR, 0, round(MAX_LASER_POWER*100/255));
      break;
  }

  eeprom_write_block((void*)&SetupVal, 0, sizeof(SetupVal));
  OUTLCD(1);
}


// программа начальной установки
void setup()
{
  eeprom_read_block((void*)&SetupVal, 0, sizeof(SetupVal));   // считываем сохраненные параметры из EPROM
  Serial.begin(9600);                                         // Задаем скорость порта
  lcd.begin(16, 2);                                           // инициализируем LCD 2 строчки по 16 символов

  // выводим заставку чтобы понять, что контроллер перезагрузился
  lcd.clear();
  lcd.setCursor(15,0);
  lcd.print("LASER MANAGEMENT");
  lcd.setCursor(15,1);
  lcd.print("MODULE   ver.1.6");
  for (int i = 0; i < 15; i++)
      {lcd.scrollDisplayLeft();
      delay(100);}
  delay(1000);

  // инициализируем входные/выходные пины
  pinMode(DIR_LASER_PIN , INPUT);
  digitalWrite (DIR_LASER_PIN, LOW);
  pinMode(STEP_LASER_PIN, INPUT);
  digitalWrite (STEP_LASER_PIN, LOW);
  pinMode(STEPX_PIN, INPUT);
  digitalWrite (STEPX_PIN, LOW);
  pinMode(STEPY_PIN, INPUT);
  digitalWrite (STEPY_PIN, LOW);
  pinMode(LASER_PIN , OUTPUT);

  // установка таймера для ШИМ управления лазером - пины D3 и D11 - 2 кГц
  TCCR2B = 0b00000011;      // x32
  TCCR2A = 0b00000011;      // fast pwm
  
  // Установка таймера и прерывания для анализа скорости движения осей X и Y
  noInterrupts();           // отключаем глобальные прерывания
  TCCR1A = 0;               // установить TCCR1A регистр в 0
  TCCR1B = 0;               // установить TCCR1B регистр в 0
  TIMSK1 = (1 << TOIE1);    // включить прерывание Timer1 overflow
  TCCR1B |= (1 << CS10);    // Установить CS10 бит так, чтобы таймер работал при тактовой частоте (прерывание каждые ~0,0041 с)

//  attachInterrupt(digitalPinToInterrupt(DIR_LASER_PIN), MoveChange, CHANGE);      // задаем прерывание по изменению DIR_LASER
  attachInterrupt(digitalPinToInterrupt(STEP_LASER_PIN), BrightChange, RISING);  // задаем прерывание по заднему фронту STEP_LASER

  interrupts();             // включить глобальные прерывания

  // инициализируем переменные 
  LASER_POWER = 0;
  LASER_AXIS_POS = 0;
  POWER_CORR = 0;  

  OUTLCD(1);                             
 
 }


// основная программа
void loop()
{
  int KEY = getPressedButton();
  if (KEY == BUTTON_NONE);
  else {
    if (KEY == BUTTON_LEFT) -- PAGE;
    else if (KEY == BUTTON_RIGHT) ++ PAGE;
    else if (KEY == BUTTON_SELECT) PAGE = 3;
    PAGE = constrain(PAGE,0,3);
    OUTLCD(1);
    if (KEY == BUTTON_UP) CHNGPAR (+STP);
    else if (KEY == BUTTON_DOWN) CHNGPAR (-STP);
       }

  POWER_CORR = round(255*constrain(float(SetupVal.XY_VEL_CORR)*((X_POWER+Y_POWER)/2046.00)/100.00,0.00,float(SetupVal.XY_VEL_CORR)/100.00));    // вычисляем величину коррекции в зависимости от скорости движения по осям XY
  LASER_POWER = constrain(round (LASER_AXIS_POS + SetupVal.OFFSET-POWER_CORR), SetupVal.OFFSET, SetupVal.MAX_LOAD);                             // вычисляем текущую мощность как положение виртуальной оси/10, добавляем offset, отнимаем коррекцию и ограничиваем лимитами
  analogWrite(LASER_PIN , LASER_POWER);                                                                                                         // выдаем на лазер

  OUTLCD(0);                // выводим статистику без очистки дисплея (чтобы меньше мерцал)
}

// подпрограмма прерывания по таймеру для анализа движения по осям XY, считывает текущую активность
ISR(TIMER1_OVF_vect)
{
  X_POWER = analogRead(STEPX_PIN);
  Y_POWER = analogRead(STEPY_PIN);
}

// подпрограмма прерывания по STEP_LASER изменяет положение виртуальной оси LASER_AXIS, связанной с мощностью лазера
void BrightChange()
{
  if (digitalRead(DIR_LASER_PIN) == HIGH) LASER_AXIS_POS += 10;
  else LASER_AXIS_POS -= 10;
}
