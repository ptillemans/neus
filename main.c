﻿/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */

   /************************************************************************************************
   Name: AvnetStarterKitReferenceDesign
   Sphere OS: 20.01
   This file contains the 'main' function. Program execution begins and ends there

   Authors:
   Peter Fenn (Avnet Engineering & Technology)
   Brian Willess (Avnet Engineering & Technology)

   Purpose:
   Using the Avnet Azure Sphere Starter Kit demonstrate the following features

   1. Read X,Y,Z accelerometer data from the onboard LSM6DSO device using the I2C Interface
   2. Read X,Y,Z Angular rate data from the onboard LSM6DSO device using the I2C Interface
   3. Read the barometric pressure from the onboard LPS22HH device using the I2C Interface
   4. Read the temperature from the onboard LPS22HH device using the I2C Interface
   5. Read the state of the A and B buttons
   6. Read BSSID address, Wi-Fi AP SSID, Wi-Fi Frequency
   *************************************************************************************************
      Connected application features: When connected to Azure IoT Hub or IoT Central
   *************************************************************************************************
   7. Send X,Y,Z accelerometer data to Azure
   8. Send barometric pressure data to Azure
   9. Send button state data to Azure
   10. Send BSSID address, Wi-Fi AP SSID, Wi-Fi Frequency data to Azure
   11. Send the application version string to Azure
   12. Control user RGB LEDs from the cloud using device twin properties
   13. Control optional Relay Click relays from the cloud using device twin properties
   14. Send Application version up as a device twin property

   	 
   *************************************************************************************************/

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h> 
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <math.h>

// applibs_versions.h defines the API struct versions to use for applibs APIs.
#include "applibs_versions.h"
#include "epoll_timerfd_utilities.h"
#include "i2c.h"
#include "hw/avnet_mt3620_sk.h"
#include "deviceTwin.h"
#include "azure_iot_utilities.h"
#include "connection_strings.h"
#include "build_options.h"

#include <applibs/log.h>
#include <applibs/i2c.h>
#include <applibs/gpio.h>
#include <applibs/wificonfig.h>
#include <azureiot/iothub_device_client_ll.h>

#include "oled.h"

#ifdef M0_INTERCORE_COMMS
//// ADC connection
#include <sys/time.h>
#include <sys/socket.h>
#include <applibs/application.h>
#endif 

// Provide local access to variables in other files
extern twin_t twinArray[];
extern int twinArraySize;
extern IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle;

#ifdef OLED_SD1306
extern sensor_var sensor_data;
extern network_var network_data;
extern float light_sensor;
#endif 

// Support functions.
static void TerminationHandler(int signalNumber);
static int InitPeripheralsAndHandlers(void);
static void ClosePeripheralsAndHandlers(void);

// File descriptors - initialized to invalid value
int epollFd = -1;
extern int accelTimerFd;
static int buttonPollTimerFd = -1;
static int buttonAGpioFd = -1;
static int buttonBGpioFd = -1;

int userLedRedFd = -1;
int userLedGreenFd = -1;
int userLedBlueFd = -1;
int wifiLedFd = -1;
int clickSocket1Relay1Fd = -1;
int clickSocket1Relay2Fd = -1;

// Azure IoT Hub/Central defines.
#define SCOPEID_LENGTH 20
char scopeId[SCOPEID_LENGTH]; // ScopeId for the Azure IoT Central application and DPS set in
						      // app_manifest.json, CmdArgs

#ifdef M0_INTERCORE_COMMS
//// ADC connection
static const char rtAppComponentId[] = "005180bc-402f-4cb3-a662-72937dbcde47";
static int sockFd = -1;
static void SendMessageToRTCore(void);
static void TimerEventHandler(EventData *eventData);
static void SocketEventHandler(EventData *eventData);
static int timerFd = -1;
uint8_t RTCore_status;

// event handler data structures. Only the event handler field needs to be populated.
static EventData timerEventData = { .eventHandler = &TimerEventHandler };
static EventData socketEventData = { .eventHandler = &SocketEventHandler };
#endif 

// Button state variables, initilize them to button not-pressed (High)
static GPIO_Value_Type buttonAState = GPIO_Value_High;
static GPIO_Value_Type buttonBState = GPIO_Value_High;

#if (defined(IOT_CENTRAL_APPLICATION) || defined(IOT_HUB_APPLICATION))
	bool versionStringSent = false;
#endif

// Define the Json string format for the accelerator button press data
static const char cstrButtonTelemetryJson[] = "{\"%s\":\"%d\"}";

// Termination state
volatile sig_atomic_t terminationRequired = false;

/// <summary>
///     Signal handler for termination requests. This handler must be async-signal-safe.
/// </summary>
static void TerminationHandler(int signalNumber)
{
    // Don't use Log_Debug here, as it is not guaranteed to be async-signal-safe.
    terminationRequired = true;
}

/// <summary>
///     Allocates and formats a string message on the heap.
/// </summary>
/// <param name="messageFormat">The format of the message</param>
/// <param name="maxLength">The maximum length of the formatted message string</param>
/// <returns>The pointer to the heap allocated memory.</returns>
static void* SetupHeapMessage(const char* messageFormat, size_t maxLength, ...)
{
	va_list args;
	va_start(args, maxLength);
	char* message =
		malloc(maxLength + 1); // Ensure there is space for the null terminator put by vsnprintf.
	if (message != NULL) {
		vsnprintf(message, maxLength, messageFormat, args);
	}
	va_end(args);
	return message;
}

/// <summary>
///     Direct Method callback function, called when a Direct Method call is received from the Azure
///     IoT Hub.
/// </summary>
/// <param name="methodName">The name of the method being called.</param>
/// <param name="payload">The payload of the method.</param>
/// <param name="responsePayload">The response payload content. This must be a heap-allocated
/// string, 'free' will be called on this buffer by the Azure IoT Hub SDK.</param>
/// <param name="responsePayloadSize">The size of the response payload content.</param>
/// <returns>200 HTTP status code if the method name is reconginized and the payload is correctly parsed;
/// 400 HTTP status code if the payload is invalid;</returns>
/// 404 HTTP status code if the method name is unknown.</returns>
static int DirectMethodCall(const char* methodName, const char* payload, size_t payloadSize, char** responsePayload, size_t* responsePayloadSize)
{
	Log_Debug("\nDirect Method called %s\n", methodName);

	int result = 404; // HTTP status code.

	if (payloadSize < 32) {

		// Declare a char buffer on the stack where we'll operate on a copy of the payload.  
		char directMethodCallContent[payloadSize + 1];

		// Prepare the payload for the response. This is a heap allocated null terminated string.
		// The Azure IoT Hub SDK is responsible of freeing it.
		*responsePayload = NULL;  // Reponse payload content.
		*responsePayloadSize = 0; // Response payload content size.


		// Look for the haltApplication method name.  This direct method does not require any payload, other than
		// a valid Json argument such as {}.

		if (strcmp(methodName, "haltApplication") == 0) {

			// Log that the direct method was called and set the result to reflect success!
			Log_Debug("haltApplication() Direct Method called\n");
			result = 200;

			// Construct the response message.  This response will be displayed in the cloud when calling the direct method
			static const char resetOkResponse[] =
				"{ \"success\" : true, \"message\" : \"Halting Application\" }";
			size_t responseMaxLength = sizeof(resetOkResponse);
			*responsePayload = SetupHeapMessage(resetOkResponse, responseMaxLength);
			if (*responsePayload == NULL) {
				Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
				abort();
			}
			*responsePayloadSize = strlen(*responsePayload);

			// Set the terminitation flag to true.  When in Visual Studio this will simply halt the application.
			// If this application was running with the device in field-prep mode, the application would halt
			// and the OS services would resetart the application.
			terminationRequired = true;
			return result;
		}

		// Check to see if the setSensorPollTime direct method was called
		else if (strcmp(methodName, "setSensorPollTime") == 0) {

			// Log that the direct method was called and set the result to reflect success!
			Log_Debug("setSensorPollTime() Direct Method called\n");
			result = 200;

			// The payload should contain a JSON object such as: {"pollTime": 20}
			if (directMethodCallContent == NULL) {
				Log_Debug("ERROR: Could not allocate buffer for direct method request payload.\n");
				abort();
			}

			// Copy the payload into our local buffer then null terminate it.
			memcpy(directMethodCallContent, payload, payloadSize);
			directMethodCallContent[payloadSize] = 0; // Null terminated string.

			JSON_Value* payloadJson = json_parse_string(directMethodCallContent);

			// Verify we have a valid JSON string from the payload
			if (payloadJson == NULL) {
				goto payloadError;
			}

			// Verify that the payloadJson contains a valid JSON object
			JSON_Object* pollTimeJson = json_value_get_object(payloadJson);
			if (pollTimeJson == NULL) {
				goto payloadError;
			}

			// Pull the Key: value pair from the JSON object, we're looking for {"pollTime": <integer>}
			// Verify that the new timer is < 0
			int newPollTime = (int)json_object_get_number(pollTimeJson, "pollTime");
			if (newPollTime < 1) {
				goto payloadError;
			}
			else {

				Log_Debug("New PollTime %d\n", newPollTime);

				// Construct the response message.  This will be displayed in the cloud when calling the direct method
				static const char newPollTimeResponse[] =
					"{ \"success\" : true, \"message\" : \"New Sensor Poll Time %d seconds\" }";
				size_t responseMaxLength = sizeof(newPollTimeResponse) + strlen(payload);
				*responsePayload = SetupHeapMessage(newPollTimeResponse, responseMaxLength, newPollTime);
				if (*responsePayload == NULL) {
					Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
					abort();
				}
				*responsePayloadSize = strlen(*responsePayload);

				// Define a new timespec variable for the timer and change the timer period
				struct timespec newAccelReadPeriod = { .tv_sec = newPollTime,.tv_nsec = 0 };
				SetTimerFdToPeriod(accelTimerFd, &newAccelReadPeriod);
				return result;
			}
		}
		else {
			result = 404;
			Log_Debug("INFO: Direct Method called \"%s\" not found.\n", methodName);

			static const char noMethodFound[] = "\"method not found '%s'\"";
			size_t responseMaxLength = sizeof(noMethodFound) + strlen(methodName);
			*responsePayload = SetupHeapMessage(noMethodFound, responseMaxLength, methodName);
			if (*responsePayload == NULL) {
				Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
				abort();
			}
			*responsePayloadSize = strlen(*responsePayload);
			return result;
		}

	}
	else {
		Log_Debug("Payload size > 32 bytes, aborting Direct Method execution\n");
		goto payloadError;
	}

	// If there was a payload error, construct the 
	// response message and send it back to the IoT Hub for the user to see
payloadError:

	result = 400; // Bad request.
	Log_Debug("INFO: Unrecognised direct method payload format.\n");

	static const char noPayloadResponse[] =
		"{ \"success\" : false, \"message\" : \"request does not contain an identifiable "
		"payload\" }";

	size_t responseMaxLength = sizeof(noPayloadResponse) + strlen(payload);
	responseMaxLength = sizeof(noPayloadResponse);
	*responsePayload = SetupHeapMessage(noPayloadResponse, responseMaxLength);
	if (*responsePayload == NULL) {
		Log_Debug("ERROR: Could not allocate buffer for direct method response payload.\n");
		abort();
	}
	*responsePayloadSize = strlen(*responsePayload);

	return result;

}


/// <summary>
///     Handle button timer event: if the button is pressed, report the event to the IoT Hub.
/// </summary>
static void ButtonTimerEventHandler(EventData *eventData)
{

	bool sendTelemetryButtonA = false;
	bool sendTelemetryButtonB = false;

	if (ConsumeTimerFdEvent(buttonPollTimerFd) != 0) {
		terminationRequired = true;
		return;
	}

	// Check for button A press
	GPIO_Value_Type newButtonAState;
	int result = GPIO_GetValue(buttonAGpioFd, &newButtonAState);
	if (result != 0) {
		Log_Debug("ERROR: Could not read button GPIO: %s (%d).\n", strerror(errno), errno);
		terminationRequired = true;
		return;
	}

	// If the A button has just been pressed, send a telemetry message
	// The button has GPIO_Value_Low when pressed and GPIO_Value_High when released
	if (newButtonAState != buttonAState) {
		if (newButtonAState == GPIO_Value_Low) {
			Log_Debug("Button A pressed!\n");
			sendTelemetryButtonA = true;
		}
		else {
			Log_Debug("Button A released!\n");
		}
		
		// Update the static variable to use next time we enter this routine
		buttonAState = newButtonAState;
	}

	// Check for button B press
	GPIO_Value_Type newButtonBState;
	result = GPIO_GetValue(buttonBGpioFd, &newButtonBState);
	if (result != 0) {
		Log_Debug("ERROR: Could not read button GPIO: %s (%d).\n", strerror(errno), errno);
		terminationRequired = true;
		return;
	}

	// If the B button has just been pressed/released, send a telemetry message
	// The button has GPIO_Value_Low when pressed and GPIO_Value_High when released
	if (newButtonBState != buttonBState) {
		if (newButtonBState == GPIO_Value_Low) {
			// Send Telemetry here
			Log_Debug("Button B pressed!\n");
			sendTelemetryButtonB = true;

#ifdef OLED_SD1306
			//// OLED
			oled_state++;

			if (oled_state > OLED_NUM_SCREEN)
			{
				oled_state = 0;
			}
#endif 
		}
		else {
			Log_Debug("Button B released!\n");

		}

		// Update the static variable to use next time we enter this routine
		buttonBState = newButtonBState;


	}
	
	// If either button was pressed, then enter the code to send the telemetry message
	if (sendTelemetryButtonA || sendTelemetryButtonB) {

		char *pjsonBuffer = (char *)malloc(JSON_BUFFER_SIZE);
		if (pjsonBuffer == NULL) {
			Log_Debug("ERROR: not enough memory to send telemetry");
		}

		if (sendTelemetryButtonA) {
			// construct the telemetry message  for Button A
			snprintf(pjsonBuffer, JSON_BUFFER_SIZE, cstrButtonTelemetryJson, "buttonA", newButtonAState);
			Log_Debug("\n[Info] Sending telemetry %s\n", pjsonBuffer);
			AzureIoT_SendMessage(pjsonBuffer);
		}

		if (sendTelemetryButtonB) {
			// construct the telemetry message for Button B
			snprintf(pjsonBuffer, JSON_BUFFER_SIZE, cstrButtonTelemetryJson, "buttonB", newButtonBState);
			Log_Debug("\n[Info] Sending telemetry %s\n", pjsonBuffer);
			AzureIoT_SendMessage(pjsonBuffer);
		}

		free(pjsonBuffer);
	}

}

// event handler data structures. Only the event handler field needs to be populated.
static EventData buttonEventData = { .eventHandler = &ButtonTimerEventHandler };

#ifdef M0_INTERCORE_COMMS
//// ADC connection

/// <summary>
///     Handle socket event by reading incoming data from real-time capable application.
/// </summary>
static void SocketEventHandler(EventData *eventData)
{
	// Read response from real-time capable application.
	char rxBuf[32];
	union Analog_data
	{
		uint32_t u32;
		uint8_t u8[4];
	} analog_data;

	int bytesReceived = recv(sockFd, rxBuf, sizeof(rxBuf), 0);

	if (bytesReceived == -1) {
		Log_Debug("ERROR: Unable to receive message: %d (%s)\n", errno, strerror(errno));
		terminationRequired = true;
	}

	// Copy data from Rx buffer to analog_data union
	for (int i = 0; i < sizeof(analog_data); i++)
	{
		analog_data.u8[i] = rxBuf[i];
	}

	// get voltage (2.5*adc_reading/4096)
	// divide by 3650 (3.65 kohm) to get current (A)
	// multiply by 1000000 to get uA
	// divide by 0.1428 to get Lux (based on fluorescent light Fig. 1 datasheet)
	// divide by 0.5 to get Lux (based on incandescent light Fig. 1 datasheet)
	// We can simplify the factors, but for demostration purpose it's OK
	light_sensor = ((float)analog_data.u32*2.5/4095)*1000000 / (3650*0.1428);

	Log_Debug("Received %d bytes. ", bytesReceived);

	Log_Debug("\n");	
}

/// <summary>
///     Handle send timer event by writing data to the real-time capable application.
/// </summary>
static void TimerEventHandler(EventData *eventData)
{
	if (ConsumeTimerFdEvent(timerFd) != 0) {
		terminationRequired = true;
		return;
	}

	SendMessageToRTCore();
}

/// <summary>
///     Helper function for TimerEventHandler sends message to real-time capable application.
/// </summary>
static void SendMessageToRTCore(void)
{
	static int iter = 0;

	// Send "Read-ADC-%d" message to real-time capable application.
	static char txMessage[32];
	sprintf(txMessage, "Read-ADC-%d", iter++);
	Log_Debug("Sending: %s\n", txMessage);

	int bytesSent = send(sockFd, txMessage, strlen(txMessage), 0);
	if (bytesSent == -1)
	{
		Log_Debug("ERROR: Unable to send message: %d (%s)\n", errno, strerror(errno));
		terminationRequired = true;
		return;
	}
}

//// end ADC connection
#endif 

/// <summary>
///     Set up SIGTERM termination handler, initialize peripherals, and set up event handlers.
/// </summary>
/// <returns>0 on success, or -1 on failure</returns>
static int InitPeripheralsAndHandlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = TerminationHandler;
    sigaction(SIGTERM, &action, NULL);

    epollFd = CreateEpollFd();
    if (epollFd < 0) {
        return -1;
    }

#ifdef M0_INTERCORE_COMMS
	//// ADC connection
	 	
	// Open connection to real-time capable application.
	sockFd = Application_Connect(rtAppComponentId);
	if (sockFd == -1) 
	{
		Log_Debug("ERROR: Unable to create socket: %d (%s)\n", errno, strerror(errno));
		Log_Debug("Real Time Core disabled or Component Id is not correct.\n");
		Log_Debug("The program will continue without showing light sensor data.\n");
		// Communication with RT core error
		RTCore_status = 1;
		//return -1;
	}
	else
	{
		// Communication with RT core success
		RTCore_status = 0;
		// Set timeout, to handle case where real-time capable application does not respond.
		static const struct timeval recvTimeout = { .tv_sec = 5,.tv_usec = 0 };
		int result = setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
		if (result == -1)
		{
			Log_Debug("ERROR: Unable to set socket timeout: %d (%s)\n", errno, strerror(errno));
			return -1;
		}

		// Register handler for incoming messages from real-time capable application.
		if (RegisterEventHandlerToEpoll(epollFd, sockFd, &socketEventData, EPOLLIN) != 0)
		{
			return -1;
		}

		// Register one second timer to send a message to the real-time core.
		static const struct timespec sendPeriod = { .tv_sec = 1,.tv_nsec = 0 };
		timerFd = CreateTimerFdAndAddToEpoll(epollFd, &sendPeriod, &timerEventData, EPOLLIN);
		if (timerFd < 0)
		{
			return -1;
		}
		RegisterEventHandlerToEpoll(epollFd, timerFd, &timerEventData, EPOLLIN);
	}

	//// end ADC Connection
#endif 
	
	if (initI2c() == -1) {
		return -1;
	}
	
	// Traverse the twin Array and for each GPIO item in the list open the file descriptor
	for (int i = 0; i < twinArraySize; i++) {

		// Verify that this entry is a GPIO entry
		if (twinArray[i].twinGPIO != NO_GPIO_ASSOCIATED_WITH_TWIN) {

			*twinArray[i].twinFd = -1;

			// For each item in the data structure, initialize the file descriptor and open the GPIO for output.  Initilize each GPIO to its specific inactive state.
			*twinArray[i].twinFd = (int)GPIO_OpenAsOutput(twinArray[i].twinGPIO, GPIO_OutputMode_PushPull, twinArray[i].active_high ? GPIO_Value_Low : GPIO_Value_High);

			if (*twinArray[i].twinFd < 0) {
				Log_Debug("ERROR: Could not open LED %d: %s (%d).\n", twinArray[i].twinGPIO, strerror(errno), errno);
				return -1;
			}
		}
	}

	// Open button A GPIO as input
	Log_Debug("Opening Starter Kit Button A as input.\n");
	buttonAGpioFd = GPIO_OpenAsInput(AVNET_MT3620_SK_USER_BUTTON_A);
	if (buttonAGpioFd < 0) {
		Log_Debug("ERROR: Could not open button A GPIO: %s (%d).\n", strerror(errno), errno);
		return -1;
	}
	// Open button B GPIO as input
	Log_Debug("Opening Starter Kit Button B as input.\n");
	buttonBGpioFd = GPIO_OpenAsInput(AVNET_MT3620_SK_USER_BUTTON_B);
	if (buttonBGpioFd < 0) {
		Log_Debug("ERROR: Could not open button B GPIO: %s (%d).\n", strerror(errno), errno);
		return -1;
	}

	// Set up a timer to poll the buttons
	struct timespec buttonPressCheckPeriod = { 0, 1000000 };
	buttonPollTimerFd =
		CreateTimerFdAndAddToEpoll(epollFd, &buttonPressCheckPeriod, &buttonEventData, EPOLLIN);
	if (buttonPollTimerFd < 0) {
		return -1;
	}
	
	// Tell the system about the callback function that gets called when we receive a device twin update message from Azure
	AzureIoT_SetDeviceTwinUpdateCallback(&deviceTwinChangedHandler);
	
	// Tell the system about the callback function to call when we receive a Direct Method message from Azure
	AzureIoT_SetDirectMethodCallback(&DirectMethodCall);

    return 0;
}

/// <summary>
///     Close peripherals and handlers.
/// </summary>
static void ClosePeripheralsAndHandlers(void)
{
    Log_Debug("Closing file descriptors.\n");
    
	closeI2c();
    CloseFdAndPrintError(epollFd, "Epoll");
	CloseFdAndPrintError(buttonPollTimerFd, "buttonPoll");
	CloseFdAndPrintError(buttonAGpioFd, "buttonA");
	CloseFdAndPrintError(buttonBGpioFd, "buttonB");

	// Traverse the twin Array and for each GPIO item in the list the close the file descriptor
	for (int i = 0; i < twinArraySize; i++) {

		// Verify that this entry has an open file descriptor
		if (twinArray[i].twinGPIO != NO_GPIO_ASSOCIATED_WITH_TWIN) {

			CloseFdAndPrintError(*twinArray[i].twinFd, twinArray[i].twinKey);
		}
	}
}

/// <summary>
///     Main entry point for this application.
/// </summary>
int main(int argc, char *argv[])
{
	// Variable to help us send the version string up only once
	bool networkConfigSent = false;
	char ssid[128];
	uint32_t frequency;
	char bssid[20];
	
	// Clear the ssid array
	memset(ssid, 0, 128);

#if (defined(IOT_CENTRAL_APPLICATION) || defined(IOT_HUB_APPLICATION))
	if (argc == 2) {
		Log_Debug("Setting Azure Scope ID %s\n", argv[1]);
		strncpy(scopeId, argv[1], SCOPEID_LENGTH);
	}
	else {
		Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs\n");
		return -1;
	}
#endif 

	Log_Debug("Avnet Starter Kit Simple Reference Application starting.\n");
    if (InitPeripheralsAndHandlers() != 0) {
        terminationRequired = true;
    }

    // Use epoll to wait for events and trigger handlers, until an error or SIGTERM happens
    while (!terminationRequired) {
        if (WaitForEventAndCallHandler(epollFd) != 0) {
            terminationRequired = true;
        }

#if (defined(IOT_CENTRAL_APPLICATION) || defined(IOT_HUB_APPLICATION))
		// Setup the IoT Hub client.
		// Notes:
		// - it is safe to call this function even if the client has already been set up, as in
		//   this case it would have no effect;
		// - a failure to setup the client is a fatal error.
		if (!AzureIoT_SetupClient()) {
			Log_Debug("ERROR: Failed to set up IoT Hub client\n");
			Log_Debug("ERROR: Verify network connection and Azure Resource configurations\n");
		}
#endif 

		WifiConfig_ConnectedNetwork network;
		int result = WifiConfig_GetCurrentNetwork(&network);

		if (result < 0) 
		{
			// Log_Debug("INFO: Not currently connected to a WiFi network.\n");
			strncpy(network_data.SSID, "Not Connected", 20);
			network_data.frequency_MHz = 0;
			network_data.rssi = 0;
		}
		else 
		{

			frequency = network.frequencyMHz;
			snprintf(bssid, JSON_BUFFER_SIZE, "%02x:%02x:%02x:%02x:%02x:%02x",
				network.bssid[0], network.bssid[1], network.bssid[2], 
				network.bssid[3], network.bssid[4], network.bssid[5]);

			if ((strncmp(ssid, (char*)&network.ssid, network.ssidLength)!=0) || !networkConfigSent) {
				
				// Clear the ssid array
				memset(ssid, 0, 128);
				strncpy(ssid, network.ssid, network.ssidLength);
				Log_Debug("SSID: %s\n", ssid);
				Log_Debug("Frequency: %dMHz\n", frequency);
				Log_Debug("bssid: %s\n", bssid);
				networkConfigSent = true;

#if (defined(IOT_CENTRAL_APPLICATION) || defined(IOT_HUB_APPLICATION))
				// Note that we send up this data to Azure if it changes, but the IoT Central Properties elements only 
				// show the data that was currenet when the device first connected to Azure.
				checkAndUpdateDeviceTwin("ssid", &ssid, TYPE_STRING, false);
				checkAndUpdateDeviceTwin("freq", &frequency, TYPE_INT, false);
				checkAndUpdateDeviceTwin("bssid", &bssid, TYPE_STRING, false);
#endif 
			}

			//// OLED
			memset(network_data.SSID, 0, WIFICONFIG_SSID_MAX_LENGTH);
			if (network.ssidLength <= SSID_MAX_LEGTH)
			{
				strncpy(network_data.SSID, network.ssid, network.ssidLength);
			}
			else
			{
				strncpy(network_data.SSID, network.ssid, SSID_MAX_LEGTH);
			}

			network_data.frequency_MHz = network.frequencyMHz;

			network_data.rssi = network.signalRssi;
		}	   		 	  	  	   	
#if (defined(IOT_CENTRAL_APPLICATION) || defined(IOT_HUB_APPLICATION))
		if (iothubClientHandle != NULL && !versionStringSent) {

			#warning "If you need to upodate the version string do so in main.c ~line 740!"
				checkAndUpdateDeviceTwin("versionString", "AvnetStarterKit-Hackster.io-V2.0", TYPE_STRING, false);
			versionStringSent = true;
		}

		// AzureIoT_DoPeriodicTasks() needs to be called frequently in order to keep active
		// the flow of data with the Azure IoT Hub
		AzureIoT_DoPeriodicTasks();
#endif
    }

    ClosePeripheralsAndHandlers();
    Log_Debug("Application exiting.\n");
    return 0;
}
