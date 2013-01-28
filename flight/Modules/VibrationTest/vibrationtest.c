/**
 ******************************************************************************
 *
 * @file       vibrationtest.c
 * @author     Tau Labs, http://www.taulabs.org Copyright (C) 2013.
 * @brief      VibrationTest module to be used as a template for actual modules.
 *             Event callback version.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Input objects: ExampleObject1, ExampleSettings
 * Output object: ExampleObject2
 *
 * This module executes in response to ExampleObject1 updates. When the
 * module is triggered it will update the data of ExampleObject2.
 *
 * No threads are used in this example.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "openpilot.h"
#include "accels.h"
#include "histogram.h"
#include "modulesettings.h"
#include "vibrationtestsettings.h"
#include "arm_math.h"
#include "accessorydesired.h"


// Private constants
#define ACCEL_COMPLEX_BUFFER_LENGTH (fft_window_size*2)   // The buffer is complex, so it needs to have twice the elements as its length

#define STACK_SIZE_BYTES (200 + 460 + (13*ACCEL_COMPLEX_BUFFER_LENGTH)) //This value has been calculated to leave 200 bytes of stack space, no matter the fft_window_size
#define TASK_PRIORITY (tskIDLE_PRIORITY+1)

// Private variables
static xTaskHandle taskHandle;
static bool module_enabled = false;
static uint16_t fft_window_size;
static bool access_accels=false;
static uint16_t accels_sum_count=0;
static float accels_data_sum_x=0;
static float accels_data_sum_y=0;
static float accels_data_sum_z=0;

// Private functions
static void VibrationTestTask(void *parameters);
static void accelsUpdatedCb(UAVObjEvent * objEv);

/**
 * Start the module, called on startup
 */
static int32_t VibrationTestStart(void)
{
	
	if (!module_enabled)
		return -1;

	//Add callback for averaging accelerometer data
	AccelsConnectCallback(&accelsUpdatedCb);
	
	// Start main task
	xTaskCreate(VibrationTestTask, (signed char *)"VibrationTest", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_VIBRATIONTEST, taskHandle);
	return 0;
}

/**
 * Initialise the module, called on startup
 */

static int32_t VibrationTestInitialize(void)
{
	ModuleSettingsInitialize();
	
#ifdef MODULE_VibrationTest_BUILTIN
	module_enabled = true;
#else
	uint8_t module_state[MODULESETTINGS_STATE_NUMELEM];
	ModuleSettingsStateGet(module_state);
	if (module_state[MODULESETTINGS_STATE_VIBRATIONTEST] == MODULESETTINGS_STATE_ENABLED) {
		module_enabled = true;
	} else {
		module_enabled = false;
	}
#endif
	
	if (!module_enabled) //If module not enabled...
		return -1;

	// Initialize UAVOs
	VibrationTestSettingsInitialize();
	HistogramInitialize();
	
	//Get the FFT window size
	VibrationTestSettingsFFTWindowSizeOptions fft_window_size_enum;
	VibrationTestSettingsFFTWindowSizeGet(&fft_window_size_enum);
	switch (fft_window_size_enum) {
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_16:
			fft_window_size = 16;
			break;
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_64:
			fft_window_size = 64;
			break;
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_256:
			fft_window_size = 256;
			break;
		case VIBRATIONTESTSETTINGS_FFTWINDOWSIZE_1024:
			fft_window_size = 1024;
			break;
		default:
			//This represents a serious configuration error. Do not start module.
			module_enabled = false;
			return -1;
			break;
	}
	
	return 0;
	
}
MODULE_INITCALL(VibrationTestInitialize, VibrationTestStart)

static void VibrationTestTask(void *parameters)
{
#define MAX_BLOCKSIZE   2048
	
	portTickType lastSysTime;
	uint8_t sample_count;
		
	float accel_buffer_x[ACCEL_COMPLEX_BUFFER_LENGTH]; //These buffers are complex numbers, so they are twice
	float accel_buffer_y[ACCEL_COMPLEX_BUFFER_LENGTH]; // as long as the number of samples, and  complex part 
	float accel_buffer_z[ACCEL_COMPLEX_BUFFER_LENGTH]; // is always 0.
	
/** These values are useful to understand about the fourier transform performed by this module.
	float freq_sample = 1.0f/(sampleRate_ms / portTICK_RATE_MS);
	float freq_nyquist = f_s/2.0f;
	uint16_t num_samples = fft_window_size;
 */

	//Create histogram bin instances. Start from i=1 because the first instance is generated by 
	// HistogramInitialize(). Generate three times the length because there are three vectors.
	// Generate half the length because the FFT output is symmetric about the mid-frequency, so
	// there's no point in using memory additional memory.
	for (int i=1; i<3*(fft_window_size>>1); i++) {
		HistogramCreateInstance();
	}
	
	// Main task loop
	HistogramData histogramData;
	sample_count = 0;
	lastSysTime = xTaskGetTickCount();
	while(1)
	{
		uint16_t sampleRate_ms;
		VibrationTestSettingsSampleRateGet(&sampleRate_ms);
		sampleRate_ms = sampleRate_ms > 0 ? sampleRate_ms : 1; //Ensure sampleRate never is 0.
		
		vTaskDelayUntil(&lastSysTime, sampleRate_ms / portTICK_RATE_MS);

		//Only read the samples if there are new ones
		if(accels_sum_count){
			access_accels=true; //This keeps the callback from altering the accelerometer sums
			
			accel_buffer_x[sample_count*2]=accels_data_sum_x/accels_sum_count;
			accel_buffer_y[sample_count*2]=accels_data_sum_y/accels_sum_count;
			accel_buffer_z[sample_count*2]=accels_data_sum_z/accels_sum_count;
				
			//Reset the accumulators
			accels_data_sum_x=0;
			accels_data_sum_y=0;
			accels_data_sum_z=0;
			accels_sum_count=0;
				
			access_accels=false; //Return control to the callback
			}
		else {
			//If there are no new samples, go back to the beginning
			continue;
		}
		
		//Set complex part to 0
		accel_buffer_x[sample_count*2+1]=0;
		accel_buffer_y[sample_count*2+1]=0;
		accel_buffer_z[sample_count*2+1]=0;

		//Advance sample and reset when at buffer end
		sample_count++;
		if (sample_count >= fft_window_size) {
			sample_count=0;
		}
		
		//Only process once the samples are filled. This could be done continuously, but this way is probably easier on the processor
		if (sample_count==0) {
			
			float fft_output[fft_window_size>>1]; //Output is symmetric, so no need to store second half of output
			arm_cfft_radix4_instance_f32 cfft_instance;
			arm_status status;
			
			/* Initialize the CFFT/CIFFT module */
			status = ARM_MATH_SUCCESS;
			bool ifftFlag = false;
			bool doBitReverse = 1;
			status = arm_cfft_radix4_init_f32(&cfft_instance, fft_window_size, ifftFlag, doBitReverse);
			
			//Perform the DFT on each of three axes
			for (int i=0; i < 3; i++) {
				if (status == ARM_MATH_SUCCESS) {
					
					//Create pointer and assign buffer vectors to it
					float *ptrCmplxVec;
					
					switch (i) {
						case 0:
							ptrCmplxVec=accel_buffer_x;
							break;
						case 1:
							ptrCmplxVec=accel_buffer_y;
							break;
						case 2:
							ptrCmplxVec=accel_buffer_z;
							break;
						default:
							//Whoops, this is a major error, leave before we overwrite memory
							continue;
					}
					
					// Process the data through the CFFT/CIFFT module
					arm_cfft_radix4_f32(&cfft_instance, ptrCmplxVec);
					
					// Process the data through the Complex Magnitude Module for calculating the magnitude at each bin.
					arm_cmplx_mag_f32(ptrCmplxVec, fft_output, fft_window_size>>1);
					
					//Write output to UAVO
					for (int j=0; j<(fft_window_size>>1); j++) { //Only output the first half of the object, since the second half is symmetric
						//Assertion check that we are not trying to write to instances that don't exist
						if (j+i*(fft_window_size>>1) >= UAVObjGetNumInstances(HistogramHandle()))
							continue;
						
						histogramData.BinValue = fft_output[j];
						HistogramInstSet(j+i*(fft_window_size>>1), &histogramData);
					}
				}
			}
		}
	}
}


/**
 * Accumulate accelerometer data. This would be a great place to add a 
 * high-pass filter, in order to eliminate the DC bias from gravity.
 */

static void accelsUpdatedCb(UAVObjEvent * objEv) 
{
	if(!access_accels){
		AccelsData accels_data;
		AccelsGet(&accels_data);
		
		accels_data_sum_x+=accels_data.x;
		accels_data_sum_y+=accels_data.y;
		accels_data_sum_z+=accels_data.z;
		
		accels_sum_count++;
	}
}
