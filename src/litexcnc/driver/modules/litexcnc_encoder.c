/********************************************************************
* Description:  litexcnc_encoder.c
*               A Litex-CNC component that canm be used to measure 
*               position by counting the pulses generated by a 
*               quadrature encoder. 
*
* Author: Peter van Tol <petertgvantol AT gmail DOT com>
* License: GPL Version 2
*    
* Copyright (c) 2022 All rights reserved.
*
********************************************************************/

/** This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General
    Public License as published by the Free Software Foundation.
    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

    THE AUTHORS OF THIS LIBRARY ACCEPT ABSOLUTELY NO LIABILITY FOR
    ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE
    TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of
    harming persons must have provisions for completely removing power
    from all motors, etc, before persons enter any danger area.  All
    machinery must be designed to comply with local and national safety
    codes, and the authors of this software can not, and do not, take
    any responsibility for such compliance.

    This code was written as part of the LiteX-CNC project.
*/
#include <limits.h>
#include "hal.h"
#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"

#include "litexcnc_encoder.h"

/** 
 * An array holding all instance for the module. As each boarf normally have a 
 * single instance of a type, this number coincides with the number of boards
 * which are supported by LitexCNC
 */
static litexcnc_encoder_t *instances[MAX_INSTANCES];
static int num_instances = 0;

/**
 * Parameter which contains the registration of this module woth LitexCNC 
 */
static litexcnc_module_registration_t *registration;

int register_encoder_module(void) {
    registration = (litexcnc_module_registration_t *)hal_malloc(sizeof(litexcnc_module_registration_t));
    registration->id = 0x656e635f; /** The string `enc_` in hex */
    rtapi_snprintf(registration->name, sizeof(registration->name), "encoder");
    registration->initialize = &litexcnc_encoder_init;
    registration->required_write_buffer = &required_write_buffer;
    registration->required_read_buffer  = &required_read_buffer;
    return litexcnc_register_module(registration);
}
EXPORT_SYMBOL_GPL(register_encoder_module);


int rtapi_app_main(void) {
    // Show some information on the module being loaded
    LITEXCNC_PRINT_NO_DEVICE(
        "Loading Litex Encoder module version %u.%u.%u\n", 
        LITEXCNC_ENCODER_VERSION_MAJOR, 
        LITEXCNC_ENCODER_VERSION_MINOR, 
        LITEXCNC_ENCODER_VERSION_PATCH
    );

    // Initialize the module
    comp_id = hal_init(LITEXCNC_ENCODER_NAME);
    if(comp_id < 0) return comp_id;

    // Register the module with LitexCNC (NOTE: LitexCNC should be loaded first)
    int result = register_encoder_module();
    if (result<0) return result;

    // Report GPIO is ready to be used
    hal_ready(comp_id);

    return 0;
}


void rtapi_app_exit(void) {
    hal_exit(comp_id);
    LITEXCNC_PRINT_NO_DEVICE("LitexCNC Encoder module driver unloaded \n");
}

size_t single_dword_buffer(litexcnc_encoder_t *encoder_module) {
    return (((encoder_module->num_instances)>>5) + ((encoder_module->num_instances & 0x1F)?1:0)) * 4;
}


size_t required_write_buffer(void *instance) {
    static litexcnc_encoder_t *encoder;
    encoder = (litexcnc_encoder_t *) instance;
    // Each encoder has 1 bit on both the Index Enable and Reset Index registers. The
    // register is by default a DWORD (32 bits). When there are more then 32 encoders,
    // another DWORD is added to both registers. Because every encoder requires two bits,
    // there is a factor 2 at the end of the calculation.
    return single_dword_buffer(encoder) * 2;
}


size_t required_read_buffer(void *instance) {
    static litexcnc_encoder_t *encoder;
    encoder = (litexcnc_encoder_t *) instance;
    // Each encoder has 1 bit as Index Pulse register. The register is by default a DWORD 
    // (32 bits). When there are more then 32 encoders, another DWORD is added to the 
    // registers. For each encoder there is also a struct with the number of counts 
    // retrieved.
    return single_dword_buffer(encoder) + num_instances * sizeof(litexcnc_encoder_instance_read_data_t);
}


size_t litexcnc_encoder_init(litexcnc_module_instance_t **module, litexcnc_t *litexcnc, uint8_t **config) {

    int r;
    char base_name[HAL_NAME_LEN + 1];   // i.e. <board_name>.<board_index>.encoder.<encoder_index>
    char name[HAL_NAME_LEN + 1];        // i.e. <base_name>.<pin_name>

    // Create structure in memory
    (*module) = (litexcnc_module_instance_t *)hal_malloc(sizeof(litexcnc_module_instance_t));
    (*module)->prepare_write = &litexcnc_encoder_prepare_write;
    (*module)->process_read  = &litexcnc_encoder_process_read;
    (*module)->instance_data = hal_malloc(sizeof(litexcnc_encoder_t));
        
    // Cast from void to correct type and store it
    litexcnc_encoder_t *encoder = (litexcnc_encoder_t *) (*module)->instance_data;
    instances[num_instances] = encoder;
    num_instances++;

    // Store the amount of pwm instances on this board and allocate HAL shared memory
    encoder->num_instances = be32toh(*(uint32_t*)*config);
    encoder->instances = (litexcnc_encoder_instance_t *)hal_malloc(encoder->num_instances * sizeof(litexcnc_encoder_instance_t));
    if (encoder->instances == NULL) {
        LITEXCNC_ERR_NO_DEVICE("Out of memory!\n");
        return -ENOMEM;
    }
    (*config) += 4;

    for (size_t i=0; i<encoder->num_instances; i++) {
        // Get pointer to the enoder instance
        litexcnc_encoder_instance_t *instance = &(encoder->instances[i]);
        
        // Create the basename
        LITEXCNC_CREATE_BASENAME("encoder", i)

        // Create the pins
        LITEXCNC_CREATE_HAL_PIN("raw-counts", s32, HAL_OUT, &(instance->hal.pin.raw_counts))
        LITEXCNC_CREATE_HAL_PIN("counts", s32, HAL_OUT, &(instance->hal.pin.counts))
        LITEXCNC_CREATE_HAL_PIN("reset", bit, HAL_IO, &(instance->hal.pin.reset))
        LITEXCNC_CREATE_HAL_PIN("index-enable", bit, HAL_IN, &(instance->hal.pin.index_enable))
        LITEXCNC_CREATE_HAL_PIN("index-pulse", bit, HAL_OUT, &(instance->hal.pin.index_pulse))
        LITEXCNC_CREATE_HAL_PIN("position", float, HAL_OUT, &(instance->hal.pin.position))
        LITEXCNC_CREATE_HAL_PIN("velocity", float, HAL_OUT, &(instance->hal.pin.velocity))
        LITEXCNC_CREATE_HAL_PIN("velocity-rpm", float, HAL_OUT, &(instance->hal.pin.velocity_rpm))
        LITEXCNC_CREATE_HAL_PIN("overflow-occurred", bit, HAL_OUT, &(instance->hal.pin.overflow_occurred))

        // Create the params
        LITEXCNC_CREATE_HAL_PARAM("position-scale", float, HAL_RW, &(instance->hal.param.position_scale))
        LITEXCNC_CREATE_HAL_PARAM("x4-mode", bit, HAL_RW, &(instance->hal.param.x4_mode))
    }

    // Succes!
    return 0;
}


int litexcnc_encoder_process_read(void *module, uint8_t **data, int period) {
    /* ENCODER PREPARE WRITE
     * This function processes the data which is received to the FPGA:
     *  - index_pulse (shared) and counts are received and stored;
     *  - position is calculated based on the counts
     *  - velocity is calculated based on the change in position
     *
     * When there are no encoders defined, this function direclty returns.
     */
    static litexcnc_encoder_t *encoder;
    encoder = (litexcnc_encoder_t *) module;
        
    // Sanity check: are there any instances of the encoder defined in the config?
    if (encoder->num_instances == 0) {
        return 0;
    }

    // Global check for changed variables and pre-calucating data
    if (encoder->memo.period != period) { 
        // - Calculate the reciprocal once here, to avoid multiple divides later
        encoder->data.recip_dt = 1.0 / (period * 0.000000001);
        encoder->memo.period = period;
    }

    // Index pulse (shared register)
    static uint8_t mask;
    static uint8_t index_pulse;
    mask = 0x80;
    for (size_t i=single_dword_buffer(encoder)*8; i>0; i--) {
        // The counter i can have a value outside the range of possible instances. We only
        // should add data to existing instances
        if (i < encoder->num_instances) {
            index_pulse = (*(*data) & mask)?1:0;
            // Reset the index enable on positive edge of the index pulse
            // NOTE: the FPGA only sets the index pulse when a raising flank has been detected
            if (index_pulse) {
                *(encoder->instances[i-1].hal.pin.index_enable) = 0;
            }
            // Set the index pulse
            *(encoder->instances[i-1].hal.pin.index_pulse) = index_pulse;
        }
        // Modify the mask for the next. When the mask is zero (happens in case of a 
        // roll-over), we should proceed to the next byte and reset the mask.
        mask >>= 1;
        if (!mask) {
            mask = 0x80;  // Reset the mask
            (*data)++; // Proceed the buffer to the next element
        }
    }

    // Process all instances:
    // - read data
    // - calculate derived data
    for (size_t i=0; i < encoder->num_instances; i++) {
        // Get pointer to the stepgen instance
        litexcnc_encoder_instance_t *instance = &(encoder->instances[i]);

        // Instance check for changed variables and pre-calucating data
        // - position scnale
        if (instance->hal.param.position_scale != instance->memo.position_scale) {
            // Prevent division by zero
            if ((instance->hal.param.position_scale > -1e-20) && (instance->hal.param.position_scale < 1e-20)) {
		        // Value too small, take a safe value
		        instance->hal.param.position_scale = 1.0;
	        }
            instance->data.position_scale_recip = 1.0 / instance->hal.param.position_scale;
            instance->memo.position_scale = instance->hal.param.position_scale; 
        }

        // Read the data and store it on the instance
        // - store the previous counts (required for roll-over detection)
        int32_t counts_old = *(instance->hal.pin.raw_counts);
        // - convert received data to struct
        litexcnc_encoder_instance_read_data_t instance_data;
        memcpy(&instance_data, *data, sizeof(litexcnc_encoder_instance_read_data_t));
        *data += sizeof(litexcnc_encoder_instance_read_data_t);

        // - store the counts from the FPGA to the driver (keep in mind the endianess).
        *(instance->hal.pin.raw_counts) = (int32_t)be32toh((uint32_t)instance_data.counts);

        // - take into account whether we are in x4_mode or not.
        *(instance->hal.pin.counts) = *(instance->hal.pin.raw_counts);
        if (!instance->hal.param.x4_mode) {
            *(instance->hal.pin.counts) = *(instance->hal.pin.counts) / 4;
        }

        // Reset mechanism
        if (*(instance->hal.pin.reset)) {
            // Store the position where the reset occurred and clear any overflow flags
            *(instance->hal.pin.overflow_occurred) = false;
            instance->memo.position_reset = *(instance->hal.pin.counts);
            // Ensure that a new roll-over does not happen in this step
            counts_old = *(instance->hal.pin.raw_counts);
            // Reset the reset pin
            *(instance->hal.pin.reset) = 0;
        }

        // Apply the reset offset
        *(instance->hal.pin.counts) -= instance->memo.position_reset;

        // Calculate the new position based on the counts
        // - store the previous position (requered for the velocity calculation)
        float position_old = *(instance->hal.pin.position);
        // - when an index pulse has been received the roll-over protection is disabled,
        //   as it is known the encoder is reset to 0 and it is not possible to roll-over
        //   within one period (assumption is that the period is less then 15 minutes).
        if (*(instance->hal.pin.index_pulse)) {
            *(instance->hal.pin.position) = *(instance->hal.pin.counts) * instance->data.position_scale_recip;
            *(instance->hal.pin.overflow_occurred) = false;
        } else {
            // Roll-over detection; it assumed when the the difference between previous value
            // and next value is larger then 0.5 times the max-range, that roll-over has occurred.
            // In this case, we switch to a incremental calculation of the position. This method is
            // less accurate then the absolute calculation of the position. Once overflow has
            // occurred, the only way to reset to absolute calculation is by the occurrence of a
            // index_pulse.
            int64_t difference = (int64_t) *(instance->hal.pin.raw_counts) - counts_old;
            if ((difference < INT_MIN) || (difference > INT_MAX)) {
                // When overflow occurs, the difference will be in order magnitude of 2^32-1, however
                // a signed integer can only allow for changes of half that size. Because we calculate
                // in 64-bit, we can detect changes which are larger and thus correct for that.
                *(instance->hal.pin.overflow_occurred) = true;
                // Compensate for the roll-over
                if (difference < 0) {
                    difference += UINT_MAX;
                } else {
                    difference -= UINT_MAX;
                }
                // Compensation if not in X4 mode
                if (!instance->hal.param.x4_mode) {
                    difference = difference / 4;
                }  
            }
            if (*(instance->hal.pin.overflow_occurred)) {
                *(instance->hal.pin.position) = *(instance->hal.pin.position) + difference * instance->data.position_scale_recip;
            } else {
                *(instance->hal.pin.position) = *(instance->hal.pin.counts) * instance->data.position_scale_recip;
            }
        }

        // Calculate the new speed based on the new position (running average). The
        // running average is not modified when an index-pulse is received, as this
        // means there is a large jump in position and thus to a large theoretical 
        // speed.
        if (!(*(instance->hal.pin.index_pulse))) {
            // Replace the element in the array
            instance->memo.velocity[encoder->memo.velocity_pointer] = (*(instance->hal.pin.position) - position_old) * encoder->data.recip_dt;
            // Sum the array and divide by the size of the array
            float average = 0.0;
            for (size_t j=0; j < LITEXCNC_ENCODER_POSITION_AVERAGE_SIZE; j++) {average += instance->memo.velocity[j];};
            *(instance->hal.pin.velocity) = average * LITEXCNC_ENCODER_POSITION_AVERAGE_RECIP;
            *(instance->hal.pin.velocity_rpm) = *(instance->hal.pin.velocity) * 60.0;
            // Increase the pointer to the next element, revert to the beginning of
            // the array
            if (encoder->memo.velocity_pointer++ >= LITEXCNC_ENCODER_POSITION_AVERAGE_SIZE) {encoder->memo.velocity_pointer=0;};
        }
    }

    return 0;
}


int litexcnc_encoder_prepare_write(void *module, uint8_t **data, int period) {
    /* ENCODER PREPARE WRITE
     * This function assembles the data which is written to the FPGA:
     *  - the `index enable`-flag, as set by the hal pin;
     *  - the `reset index pulse`-flag. At this moment the index pulse is automatically
     *    reset by the driver as soon as the pin_Z is HIGH has been read. This means that
     *    during the next cycle of the thread the pin will be read as LOW. Possibly this
     *    method can be refined optionally let the user reset the pin_Z manually. This
     *    might be necessary when there are two parallel threads running at the same time. 
     *
     * When there are no encoders defined, this function direclty returns.
     */
    static litexcnc_encoder_t *encoder;
    encoder = (litexcnc_encoder_t *) module;

    // Sanity check: are there any instances of the encoder defined in the config?
    if (encoder->num_instances == 0) {
        return 0;
    }

    // Declaration of shared variables
    uint8_t mask;

    // Index enable (shared register)
    mask = 0x80;
    for (size_t i=single_dword_buffer(encoder)*8; i>0; i--) {
        // The counter i can have a value outside the range of possible instances. We only
        // should add data from existing instances
        if (i < encoder->num_instances) {
            *(*data) |= *(encoder->instances[i-1].hal.pin.index_enable)?mask:0;
        }
        // Modify the mask for the next. When the mask is zero (happens in case of a 
        // roll-over), we should proceed to the next byte and reset the mask.
        mask >>= 1;
        if (!mask) {
            mask = 0x80;  // Reset the mask
            (*data)++; // Proceed the buffer to the next element
        }
    }

    // Reset index pulse (shared register)
    mask = 0x80;
    for (size_t i=single_dword_buffer(encoder)*8; i>0; i--) {
        // The counter i can have a value outside the range of possible instances. We only
        // should add data from existing instances
        if (i < encoder->num_instances) {
            *(*data) |= *(encoder->instances[i-1].hal.pin.index_pulse)?mask:0;
        }
        // Modify the mask for the next. When the mask is zero (happens in case of a 
        // roll-over), we should proceed to the next byte and reset the mask.
        mask >>= 1;
        if (!mask) {
            mask = 0x80;  // Reset the mask
            (*data)++; // Proceed the buffer to the next element
        }
    }

    return 0;

}
