/*  Heading Synchronization Code
    Based on Sync_XBee042417
    Roomba achieves phase change using a constant frequency change.
    Modification to DH_Turn() function
    
    Last Updated: 5/15/2017
*/

#include <SoftwareSerial.h>
#include <SPI.h>
#include "Wire.h"
#include "compassCUCI.h"

#define address 0x1E //0011110b, I2C 7bit address of HMC5883

const int rxPin = 3;            // Communication links to Roomba
const int txPin = 4;
const int ddPin = 5;
const int yellowPin = 6;        // On when the Roomba is turning.
const int redPin = 7;           // On when the Roomba is told to turn 0 mm/s.
const int greenPin = 8;         // On when the Roomba sends a pulse. (very fast, may not see)

// Communication links to XBee module
const int transmit_pin = 11;    // to pin 3 of XBee (TX)
const int receive_pin = 12;     // to pin 2 of Xbee (RX)

/* Global variables for transmission and receiving */
unsigned char message = 0;

/* Global variables needed to implement turn functions */
float angle;                      // Heading of Roomba (found from digital compass)
float counter;                    // Global counter for Roomba (works with angle to compute "phase")
float d_angle;                    // Change in angle that Roomba will turn (updates each cycle)
float DesiredHeading;             // Heading set point of Roomba;
int spinValue;                    // Speed of wheels in mm/s to spin toward DesiredHeading
int forward = 0;                  // Speed in mm/s that Roomba wheels turn to move forward - must be in range [-400,400]
const float pi = 3.1415926;       // PI to 7 decimal places

/* Adjustable Synchronization Parameters */
const float RATIO = 0.5;          // Ratio for amount to turn - must be in range (0 1]
const float EPSILON = 1.0;        // (ideally) Smallest resolution of digital compass (used in PRC function) - 5.0
boolean DHFlag = false;           // Desired Heading function indicator (was the last command not to turn?)
const float omegaA = 0.3;         // Percentage of phase change (constant frequency method)

/* Roomba parameters */
const int WHEELDIAMETER = 72;     // 72 millimeter wheel diameter
const int WHEELSEPARATION = 235;  // 235 millimeters between center of main wheels
const float ENCODER = 508.8;      // 508.8 Counts per wheel revolution

/* Sync Counter Setup */
unsigned long millisCounter;                    // Base time for Counter difference calculation
const float counterspeed = 36;                  // Number of times per second that the counter increments - range (0,1000]
                                                // Adjust this value to vary the speed/frequency of the counter
const float millisRatio = counterspeed / 1000;  // Counter increments per millisecond
const float millisAdjust = 360000 / counterspeed; // Amount of counter adjustment per firing
const unsigned long counterAdjust = (unsigned long) millisAdjust; // Truncate to unsigned long

/* Data Parameters */
const int dataTIMER = 100;    // Number of milliseconds between each data point

/* Roomba and XBee Serial Setup */
SoftwareSerial Roomba(rxPin, txPin); // Set up communication with Roomba
SoftwareSerial XBee(receive_pin, transmit_pin); // Set up communication with Xbee

/* Data Point Collector Setup */
unsigned long deltime;
unsigned long resettime;
long sno = 0;

/* Start up Roomba */
void setup() {
  Serial.begin(115200);
  display_Running_Sketch();     // Show sketch information in the serial monitor at startup
  Serial.println("Loading...");
  pinMode(ddPin,  OUTPUT);      // sets the pins as output
  pinMode(greenPin, OUTPUT);
  pinMode(redPin, OUTPUT);
  pinMode(yellowPin, OUTPUT);
  Roomba.begin(115200);         // Declare Roomba communication baud rate.
  XBee.begin(57600);            // Declare XBee communication baud rate.
  digitalWrite(greenPin, HIGH); // say we're alive

  // set up ROI to receive commands
  Roomba.write(byte(7));  // RESTART
  delay(10000);
  Serial.print("STARTING ROOMBA.");
  Roomba.write(byte(128));  // START
  delay(50);
  Roomba.write(byte(131));  // CONTROL
  //131 - Safe Mode
  //132 - Full mode (Be ready to catch it!)

  //Light up the blue dirt detect light (Needs better comments!)
  Roomba.write(byte(139)); //Begin (power?) LED control
  Roomba.write(byte(25)); //LED Bits
  Roomba.write(byte(0)); //LED color, 0 should be green
  Roomba.write(byte(128)); //Start command, not sure why you have to send this again...
  delay(500);
  Roomba.write(byte(139)); //Same thing as above, but flash red.
  Roomba.write(byte(25));
  Roomba.write(byte(255));
  Roomba.write(byte(128));
  delay(500);
  Roomba.write(byte(139));
  Roomba.write(byte(25));
  Roomba.write(byte(255));
  Roomba.write(byte(0));
  delay(50);

  //Transmitter Setup
  //Initialize Serial and I2C communications
  Wire.begin();

  //Put the HMC5883 IC into the correct operating mode
  Wire.beginTransmission(address); //open communication with HMC5883
  Wire.write(0x02); //select mode register
  Wire.write(0x00); //continuous measurement mode
  Wire.endTransmission();
  
  /* Compass Calibration: */
  
  //Keep spinning for calibration
  compass_init(1); // Set Compass Gain
  Move(0, 75);    // Set roomba spinning to calibrate the compass
                   // Spins ~2 rotations CCW.
  compass_debug = 1; // Show Debug Code in Serial Monitor (Set to 0 to hide Debug Code)
  compass_offset_calibration(2); // Find compass axis offsets
  Move(0, 0); // Stop spinning after completing calibration

  //Receiver Setup
  Serial.println(" Setup complete");
  digitalWrite(yellowPin, HIGH);
  delay(500);
  digitalWrite(yellowPin, LOW);
  digitalWrite(greenPin, LOW);  // say we've finished setup

  // calculate spin speed value
  // standard "wheel speed" = counterspeed * pi * (WHEELSEPARATION/2) / (2 * 180);
  // spinValue = omegaA * standard "wheel speed"
  //spinValue =(int) ((omegaA*(counterspeed * pi * WHEELSEPARATION) / 360) + 0.5);
  spinValue = 15; // Approximately omegaA = 0.2; 25 -> 0.34
  /* Wait for command to initialize synchronization */
  delay(200);
  angle = Calculate_Heading();  // Throw away first calculation
  delay(200);
  while(XBee.available()){ // Clear out Xbee buffer
    message = XBee.read();
  }
  message = 0; // Clear message
  /* Initialize synchronization */
  angle = Calculate_Heading();  // Determine initial heading information
  sendPalse();                  // Send reset Palse
  resetCounters();              // Reset counter values
}

void loop() { // Swarm "Heading Synchronizaiton" Code
  /* Read angle from compass */
  angle = Calculate_Heading();        // Set angle from the compass reading
  counter = millisRatio * (long)(millis() - millisCounter);
  
  /* Send a pulse signal */
  if (angle + counter >= 360) { // If my "phase" reaches 360 degrees...
    sendPulse();                                    // Fire pulse
    millisCounter = millisCounter + counterAdjust;  // Adjust base counter.
  } // Ignore if the angle and counter are less than 360 degrees.
  
  /* Receive a pulse signal */
  recievePulse();

  /* If a pulse signal was recieved */
  if (message == 'b') {
    //Serial.println(" Reset Palse.");    // Include for debugging
    digitalWrite(redPin, HIGH);   // Notify that we received palse
    digitalWrite(greenPin, HIGH);
    resetCounters();
    digitalWrite(redPin, LOW);    // End Notify that we received palse
    digitalWrite(greenPin, LOW);
    message = 0; // Clear the message variable
  } else if (message == 'a') {
    //Serial.println(" Sync Pulse.");     // Include for debugging
    digitalWrite(yellowPin, HIGH);  // Tell me that the robot is turning
    PRC_Sync(angle + counter);      // Find desired amount of turn based on PRC for synchronization
    // Turn by d_angle
    DesiredHeading = angle + d_angle;        // Set new desired heading set point
    if (DesiredHeading < 0) {         // Normalize the heading value to between 0 and 360
      DesiredHeading += 360;
    } else if (DesiredHeading >= 360) {
      DesiredHeading -= 360;
    }    
    message = 0; // Clear the message variable
  }

  DH_Turn();                // Turn to the DesiredHeading set point

  /* Send to Serial monitor a data point */
  if (millis() - deltime >= dataTIMER) { // If 1 second = 1000 milliseconds have passed...
    deltime += dataTIMER;     // Reset base value for data points
    sno++; // Increment the data point number
    Serial.println(";");    // End row, start new row of data
    Print_Heading_Data();
  }
  
  /* Reset Counters of all Roombas every 5 minutes */
  if (millis() - resettime >= 300000) { // If it's been 5 minutes...
    sendPalse();    // Send Reset Palse
    resetCounters();// Reset Counter values
  }
}/* Go back and check everything again. Should be fast */

/* SUBROUTINES */

/* Recieve pulse from RF transmitter and save value */
void recievePulse() {
  if (XBee.available()) {  // If I receive a pulse ...
    message = XBee.read(); // Return pulse character
    //Serial.print("Received: "); // Include for debugging
    //Serial.println((char)message);
  }
}
/* Sends out a pulse when phase equals 360 degrees */
void sendPulse() {
  //Serial.println("Sync Pulse Sent.");     // Include for debugging
  digitalWrite(greenPin, HIGH); // Tell me that I'm sending a pulse
  XBee.write("a"); // Sync Pulse
  digitalWrite(greenPin, LOW);  // Tell me that I'm done sending a pulse
}

/* Sends out a palse when new Roomba finishes setup */
void sendPalse() {
  //Serial.println("Reset Pulse Sent.");    // Include for debugging
  digitalWrite(greenPin, HIGH); // Tell me that I'm sending a palse
  digitalWrite(redPin, HIGH);
  XBee.write("b"); // Reset Palse
  digitalWrite(greenPin, LOW);  // Tell me that I'm done sending a palse
  digitalWrite(redPin, LOW);
}

/* Reset ALL the counters */
void resetCounters() {
  millisCounter = millis();   // Reset base counter (on all robots)
  deltime = millis();         // Reset base value for data output
  resettime = millis();       // Reset base value for reset timer
  sno = 0;                    // Reset data point counter
  DesiredHeading = angle;     // Reset heading set point
  counter = 0;                // Reset counter value
  Serial.println();           // Print text on next line
  Print_Heading_Data();       // Display initial heading information
}

/* Determines the necessary change in heading of the robot according to the pulse received */
void PRC_Sync(float phase) {
  /* Set d_angle, the amount to turn */
  /* Red LED turns on if d_angle is set to 0 */
  if ((phase) > (360 - EPSILON)) {          // If the phase is close to where I want...
    d_angle = 0;  // Don't turn at all.
    digitalWrite(redPin, HIGH); // Indicates received pulse, but no turning.
  } else if ((phase) >= 180) {              // If phase is greater than 180 degrees...
    /* Increase Heading */
    d_angle = (360 - (phase)) * RATIO;  // Amount I want to change the heading
    digitalWrite(redPin, LOW); // Indicates the last pulse received caused a turn
  } else if ((phase) > EPSILON) {           // If phase is less than 180, but not close...
    /* Decrease Heading */
    d_angle = -1 * (phase) * RATIO; // Amount I want to change the heading
    digitalWrite(redPin, LOW); // Indicates the last pulse received caused a turn
  } else {                                  // If the phase is close to where I want.
    d_angle = 0;  // Don't turn at all.
    digitalWrite(redPin, HIGH); // Indicates received pulse, but no turning.
  }
}

/* Set Roomba spin to acheive the value of DesiredHeading */
void DH_Turn(void) {
  // DesiredHeading is the set point for the heading
  // EPSILON is desirably small (probably in range [0.5, 1])
  // Need the amount of spin to be less than (2 * # of cycles through main loop in 1 second)
     // or may need to grade the amount of spin (proportional to amount of heading change)
  if (angle < (DesiredHeading + EPSILON) && angle > (DesiredHeading - EPSILON) && DHFlag == false) {
    // If it's not moving, and I'm close enough...
    digitalWrite(yellowPin, LOW); // Say we have stopped turning.
    return; // Leave function
  }
  /*
   * DHFlag = false if last command was to not spin
   * DHFlag = true if last command was to spin
   * if (angle - DesiredHeading < EPSILON && DHFlag == false)
   *    then return from function (return to loop)
   */
  // spinValue is determined beforehand
  // Determine direction of spin
  if(DesiredHeading < EPSILON) {
    if(angle > (DesiredHeading + EPSILON) && angle < (DesiredHeading + 180) ) {
      // Spin Left (CCW)
      Move(forward, -spinValue);
      DHFlag = true;
    } else if ((angle < (360 + DesiredHeading - EPSILON)) && (angle >= (DesiredHeading + 180)) ) {
      // Spin Right (CW)
      Move(forward, spinValue);
      DHFlag = true;
    } else { // if ((360 + DesiredHeading - EPSILON) < angle < (DesiredHeading + EPSILON))
      // Stop Spinning
      Move(forward, 0);
      DHFlag = false;
      digitalWrite(yellowPin, LOW); // Say we have stopped turning.
    }
  } else if(DesiredHeading < 180) { // and DesiredHeading >= EPSILON...
    if(angle > (DesiredHeading + EPSILON) && angle < (DesiredHeading + 180) ) {
      // Spin Left (CCW)
      Move(forward, -spinValue);
      DHFlag = true;
    } else if ((angle < (DesiredHeading - EPSILON)) || (angle >= (DesiredHeading + 180)) ) {
      // Spin Right (CW)
      Move(forward, spinValue);
      DHFlag = true;
    } else { // if ((DesiredHeading - EPSILON) < angle < (DesiredHeading + EPSILON))
      // Stop Spinning
      Move(forward, 0);
      DHFlag = false;
      digitalWrite(yellowPin, LOW); // Say we have stopped turning.
    }
  } else if(DesiredHeading < (360 - EPSILON)) { // and DesiredHeading >= 180)...
    if ((angle < (DesiredHeading - EPSILON)) && (angle > (DesiredHeading - 180)) ) {
      // Spin Right (CW)
      Move(forward, spinValue);
      DHFlag = true;
    } else if ((angle > (DesiredHeading + EPSILON)) || (angle <= (DesiredHeading - 180)) ) {
      // Spin Left (CCW)
      Move(forward, -spinValue);
      DHFlag = true;
    } else {  // if ((DesiredHeading - EPSILON) < angle < (DesiredHeading + EPSILON))
      // Stop Spinning
      Move(forward, 0);
      DHFlag = false;
      digitalWrite(yellowPin, LOW); // Say we have stopped turning.
    }
  } else { // if DesiredHeading >= (360 - EPSILON)...
    if ((angle < (DesiredHeading - EPSILON)) && (angle > (DesiredHeading - 180)) ) {
      // Spin Right (CW)
      Move(forward, spinValue);
      DHFlag = true;
    } else if ((angle > (DesiredHeading + EPSILON - 360)) && (angle <= (DesiredHeading - 180)) ) {
      // Spin Left (CCW)
      Move(forward, -spinValue);
      DHFlag = true;
    } else {  // if ((DesiredHeading - EPSILON) < angle < (DesiredHeading + EPSILON))
      // Stop Spinning
      Move(forward, 0);
      DHFlag = false;
      digitalWrite(yellowPin, LOW); // Say we have stopped turning.
    } // End "else angle"
  } // End "else DesiredHeading"
} // End Function

/* General Wheel Motor command function.
    X = common wheel speed (mm/s); Y = differential wheel speed;
    X > 0 -> forward motion; Y > 0 -> CW motion
    This function allows for both turning and forward motion.
    Updated to use bitshift logic.
    Error may result if |X|+|Y| > 500 (Max value is 500)
    */
void Move(int X, int Y) {
 unsigned int RW = (X - Y);
 unsigned int LW = (X + Y);
 Roomba.write(byte(145));
 Roomba.write(byte((RW & 0xff00) >> 8));
 Roomba.write(byte(RW & 0xff));
 Roomba.write(byte((LW & 0xff00) >> 8));
 Roomba.write(byte(LW & 0xff));
 return;
}

float Calculate_Heading(void) {
  float t;
  compass_scalled_reading(); // Get Raw data from the compass
  /* Heading Calculation (y-axis front) */
  t = (atan2(compass_y_scalled,compass_x_scalled) * (180/pi) - 90);
  if (t < 0) {
    t = t + 360;
  }

  return t;
}

/* Display Heading Information to the Serial Monitor */
void Print_Heading_Data(void) {
  Serial.print(sno);      // Data point number
  Serial.print(", ");
  Serial.print(angle);    // Robot Heading
  Serial.print(", ");
  Serial.print(counter);  // Counter value
  Serial.print(", ");
  Serial.print(compass_x_scalled);  // Raw X magnetometer value
  Serial.print(", ");
  Serial.print(compass_y_scalled);  // Raw Y magnetometer value
  Serial.print(", ");
  Serial.print(compass_z_scalled);  // Raw Z magnetometer value
  Serial.print(", ");
  Serial.print(DesiredHeading);     // Desired Set point (angle should follow this)
}

/* Displays the Sketch running on the Arduino. Use at startup on all code. */
void display_Running_Sketch (void) {
  /* Find the necessary informaiton */
  String the_path = __FILE__;
  int slash_loc = the_path.lastIndexOf('\\');
  String the_cpp_name = the_path.substring(slash_loc + 1);
  /* Display to the Serial Monitor */
  Serial.print("\nSketch Name: ");
  Serial.println(the_cpp_name);
  Serial.print("Compiled on: ");
  Serial.print(__DATE__);
  Serial.print(" at ");
  Serial.print(__TIME__);
  Serial.println("\n");
}
