/*
  gcode.c - rs274/ngc parser.
  Part of Grbl

  Copyright (c) 2009 Simen Svale Skogsrud

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

/*  This code is inspired by the Arduino GCode Interpreter by Mike Ellery 
	and the NIST RS274/NGC Interpreter by Kramer, Proctor and Messina. 

  Commands omitted for the time being:
	- group 0 = {G10, G28, G30, G92, G92.1, G92.2, G92.3} (Non modal G-codes)
	- group 8 = {M7, M8, M9} coolant (special case: M7 and M8 may be active at the same time)
	- group 9 = {M48, M49} enable/disable feed and speed override switches
	- group 12 = {G54, G55, G56, G57, G58, G59, G59.1, G59.2, G59.3} coordinate system selection
	- group 13 = {G61, G61.1, G64} path control mode

  Commands intentionally not supported:
	- Canned cycles
	- Tool radius compensation
	- A,B,C-axes
	- Multiple coordinate systems
	- Evaluation of expressions
	- Variables (Parameters)
	- Multiple home locations
	- Probing
	- Override control
*/

/* 
  TinyG Notes:
  Modified Grbl to support Xmega family processors
  Modifications Copyright (c) 2010 Alden S. Hart, Jr.
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "nuts_bolts.h"
#include "gcode.h"		// Must precede config.h
#include "config.h"
#include "motion_control.h"
#include "spindle_control.h"
#include "errno.h"
#include "serial_protocol.h"

#define NEXT_ACTION_DEFAULT 	0
#define NEXT_ACTION_DWELL 		1
#define NEXT_ACTION_GO_HOME 	2

#define MOTION_MODE_RAPID_LINEAR 0 	// G0 
#define MOTION_MODE_LINEAR 		1 	// G1
#define MOTION_MODE_CW_ARC 		2  	// G2
#define MOTION_MODE_CCW_ARC 	3  	// G3
#define MOTION_MODE_CANCEL 		4 	// G80

#define PATH_CONTROL_MODE_EXACT_PATH 0
#define PATH_CONTROL_MODE_EXACT_STOP 1
#define PATH_CONTROL_MODE_CONTINOUS  2

#define PROGRAM_FLOW_RUNNING	0
#define PROGRAM_FLOW_PAUSED		1
#define PROGRAM_FLOW_COMPLETED	2

#define SPINDLE_DIRECTION_CW	0
#define SPINDLE_DIRECTION_CCW	1

struct ParserState {
	uint8_t status_code;
	uint8_t motion_mode;         	/* {G0, G1, G2, G3, G38.2, G80, G81, G82, G83, G84, G85, G86, G87, G88, G89} */
	uint8_t inverse_feed_rate_mode; /* G93, G94 */
	uint8_t inches_mode;         	/* 0 = millimeter mode, 1 = inches mode {G20, G21} */
	uint8_t absolute_mode;       	/* 0 = relative motion, 1 = absolute motion {G90, G91} */
	uint8_t program_flow;
	int spindle_direction;
	double feed_rate, seek_rate;	/* Millimeters/second */
	double position[3];				/* Where the interpreter considers the tool to be at this point in the code */
	uint8_t tool;
	int16_t spindle_speed;			/* RPM/100 */
	uint8_t plane_axis_0, plane_axis_1, plane_axis_2; // The axes of the selected plane
};

struct ParserState gc;

#define FAIL(status) gc.status_code = status;

int read_double(char *line,				//  <- string: line of RS274/NGC code being processed
                int *counter,			//  <- pointer to a counter for position on the line 
                double *double_ptr);	//  <- pointer to double to be read                  

int next_statement(char *letter, double *double_ptr, char *line, int *counter);

/* gc_init() */

void gc_init() {
	memset(&gc, 0, sizeof(gc));
	gc.feed_rate = settings.default_feed_rate/60;
	gc.seek_rate = settings.default_seek_rate/60;
	select_plane(X_AXIS, Y_AXIS, Z_AXIS);
	gc.absolute_mode = TRUE;
}


void select_plane(uint8_t axis_0, uint8_t axis_1, uint8_t axis_2) 

/* gc_init() */
{
	gc.plane_axis_0 = axis_0;
	gc.plane_axis_1 = axis_1;
	gc.plane_axis_2 = axis_2;
}


inline float to_millimeters(double value) 
{
	return(gc.inches_mode ? (value * INCHES_PER_MM) : value);
}

/* theta(double x, double y)

	Find the angle in radians of deviance from the positive y axis. 
	negative angles to the left of y-axis, positive to the right.
*/
double theta(double x, double y)
{
	double theta = atan(x/fabs(y));

	if (y>0) {
		return(theta);
	} else {
		if (theta>0) 
	    {
			return(M_PI-theta);
    	} else {
			return(-M_PI-theta);
		}
	}
}

/* gc_execute_line() - executes one line of 0-terminated G-Code. 
	The line is assumed to contain only uppercase characters and 
	signed floats (no whitespace).
*/
uint8_t gc_execute_line(char *textline) {
	int counter = 0;  
	char letter;
	double value;
	double unit_converted_value;
	double inverse_feed_rate = -1; // negative inverse_feed_rate means no inverse_feed_rate specified
	int radius_mode = FALSE;
  
	uint8_t absolute_override = FALSE; /* 1 = absolute motion for this block only {G53} */
	uint8_t next_action = NEXT_ACTION_DEFAULT; /* One of the NEXT_ACTION_-constants */
  
	double target[3], offset[3];  
  	double p = 0, r = 0;
	int int_value;
  
	clear_vector(target);
	clear_vector(offset);

	gc.status_code = GCSTATUS_OK;

  // First: parse all statements
  
	if (textline[0] == '(') { 
		return(gc.status_code); 
	}
	if (textline[0] == '/') { 	// ignore block delete
		counter++; 
	} 
  	if (textline[0] == '$') { 	// This is a parameter line intended to change EEPROM-settings
    							// Parameter lines are on the form '$4=374.3' or 
								// '$' to dump current settings
		counter = 1;
		if(textline[counter] == 0) {
			dump_settings(); return(GCSTATUS_OK); 
		}
	    read_double(textline, &counter, &p);

    	if(textline[counter++] != '=') { 
			return(GCSTATUS_UNSUPPORTED_STATEMENT); 
		}
    	read_double(textline, &counter, &value);

    	if(textline[counter] != 0) { 
			return(GCSTATUS_UNSUPPORTED_STATEMENT); 
		}
    	store_setting(p, value);
  	}
  
  // Pass 1: Commands
	while(next_statement(&letter, &value, textline, &counter)) {
    	int_value = trunc(value);
    	switch(letter) {
			case 'G':
				switch(int_value) {
					case 0: gc.motion_mode = MOTION_MODE_RAPID_LINEAR; break;
					case 1: gc.motion_mode = MOTION_MODE_LINEAR; break;
					case 2: gc.motion_mode = MOTION_MODE_CW_ARC; break;
					case 3: gc.motion_mode = MOTION_MODE_CCW_ARC; break;
					case 4: next_action = NEXT_ACTION_DWELL; break;
					case 17: select_plane(X_AXIS, Y_AXIS, Z_AXIS); break;
					case 18: select_plane(X_AXIS, Z_AXIS, Y_AXIS); break;
					case 19: select_plane(Y_AXIS, Z_AXIS, X_AXIS); break;
					case 20: gc.inches_mode = TRUE; break;
					case 21: gc.inches_mode = FALSE; break;
					case 28: case 30: next_action = NEXT_ACTION_GO_HOME; break;
					case 53: absolute_override = TRUE; break;
					case 80: gc.motion_mode = MOTION_MODE_CANCEL; break;
					case 90: gc.absolute_mode = TRUE; break;
					case 91: gc.absolute_mode = FALSE; break;
					case 93: gc.inverse_feed_rate_mode = TRUE; break;
					case 94: gc.inverse_feed_rate_mode = FALSE; break;
					default: FAIL(GCSTATUS_UNSUPPORTED_STATEMENT);
				}
				break;
      
			case 'M':
				switch(int_value) {
					case 0: case 1: gc.program_flow = PROGRAM_FLOW_PAUSED; break;
					case 2: case 30: case 60: gc.program_flow = PROGRAM_FLOW_COMPLETED; break;
					case 3: gc.spindle_direction = 1; break;
					case 4: gc.spindle_direction = -1; break;
					case 5: gc.spindle_direction = 0; break;
        			default: FAIL(GCSTATUS_UNSUPPORTED_STATEMENT);
				}
				break;

			case 'T': gc.tool = trunc(value); break;
		}
		if(gc.status_code) {
			break;
		}
	}
  
  // If there were any errors parsing this line, we will return right away with the bad news
	if (gc.status_code) { 
		return(gc.status_code); 
	}

	counter = 0;
	clear_vector(offset);
	memcpy(target, gc.position, sizeof(target)); // target = gc.position

  // Pass 2: Parameters
	while(next_statement(&letter, &value, textline, &counter)) {
		int_value = trunc(value);
		unit_converted_value = to_millimeters(value);
		switch(letter) {
			case 'F': 
				if (gc.inverse_feed_rate_mode) {
					inverse_feed_rate = unit_converted_value; // seconds per motion for this motion only
				} else {
					gc.feed_rate = unit_converted_value/60; // millimeters pr second
				}
				break;
			case 'I': case 'J': case 'K': offset[letter-'I'] = unit_converted_value; break;
			case 'P': p = value; break;
			case 'R': r = unit_converted_value; radius_mode = TRUE; break;
			case 'S': gc.spindle_speed = value; break;
			case 'X': case 'Y': case 'Z':
				if (gc.absolute_mode || absolute_override) {
					target[letter - 'X'] = unit_converted_value;
				} else {
					target[letter - 'X'] += unit_converted_value;
				}
 				break;
		}	
	}
  
  // If there were any errors parsing this line, we will return right away with the bad news
	 if (gc.status_code) {
		return(gc.status_code); 
	}
    
  // Update spindle state
	if (gc.spindle_direction) {
    	spindle_run(gc.spindle_direction, gc.spindle_speed);
	} else {
		spindle_stop();\
	}
  
  // Perform any physical actions
	switch (next_action) {
    	case NEXT_ACTION_GO_HOME: mc_go_home(); break;
		case NEXT_ACTION_DWELL: mc_dwell(trunc(p*1000)); break;
		case NEXT_ACTION_DEFAULT: 
 		switch (gc.motion_mode) {
			case MOTION_MODE_CANCEL: break;
			case MOTION_MODE_RAPID_LINEAR:
			case MOTION_MODE_LINEAR:
				mc_line(target[X_AXIS], target[Y_AXIS], target[Z_AXIS], 
						(gc.inverse_feed_rate_mode) ? 
						inverse_feed_rate : gc.feed_rate, gc.inverse_feed_rate_mode);
			break;
			case MOTION_MODE_CW_ARC: case MOTION_MODE_CCW_ARC:
				if (radius_mode) {
        /* 
          We need to calculate the center of the circle that has the designated radius and passes
          through both the current position and the target position. This method calculates the following
          set of equations where [x,y] is the vector from current to target position, d == magnitude of 
          that vector, h == hypotenuse of the triangle formed by the radius of the circle, the distance to
          the center of the travel vector. A vector perpendicular to the travel vector [-y,x] is scaled to the 
          length of h [-y/d*h, x/d*h] and added to the center of the travel vector [x/2,y/2] to form the new point 
          [i,j] at [x/2-y/d*h, y/2+x/d*h] which will be the center of our arc.
          
          d^2 == x^2 + y^2
          h^2 == r^2 - (d/2)^2
          i == x/2 - y/d*h
          j == y/2 + x/d*h
          
                                                               O <- [i,j]
                                                            -  |
                                                  r      -     |
                                                      -        |
                                                   -           | h
                                                -              |
                                  [0,0] ->  C -----------------+--------------- T  <- [x,y]
                                            | <------ d/2 ---->|
                    
          C - Current position
          T - Target position
          O - center of circle that pass through both C and T
          d - distance from C to T
          r - designated radius
          h - distance from center of CT to O
          
          Expanding the equations:

          d -> sqrt(x^2 + y^2)
          h -> sqrt(4 * r^2 - x^2 - y^2)/2
          i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2 
          j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2
         
          Which can be written:
          
          i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
          j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
          
          Which we for size and speed reasons optimize to:

          h_x2_div_d = sqrt(4 * r^2 - x^2 - y^2)/sqrt(x^2 + y^2)
          i = (x - (y * h_x2_div_d))/2
          j = (y + (x * h_x2_div_d))/2
          
        */
        
        // Calculate the change in position along each selected axis
        double x = target[gc.plane_axis_0]-gc.position[gc.plane_axis_0];
        double y = target[gc.plane_axis_1]-gc.position[gc.plane_axis_1];
        
        clear_vector(&offset);
        double h_x2_div_d = -sqrt(4 * r*r - x*x - y*y)/hypot(x,y); // == -(h * 2 / d)
        // If r is smaller than d, the arc is now traversing the complex plane beyond the reach of any
        // real CNC, and thus - for practical reasons - we will terminate promptly:
        if(isnan(h_x2_div_d)) { FAIL(GCSTATUS_FLOATING_POINT_ERROR); return(gc.status_code); }
        // Invert the sign of h_x2_div_d if the circle is counter clockwise (see sketch below)
        if (gc.motion_mode == MOTION_MODE_CCW_ARC) { h_x2_div_d = -h_x2_div_d; }
        
        /* The counter clockwise circle lies to the left of the target direction. When offset is positive,
           the left hand circle will be generated - when it is negative the right hand circle is generated.
           
           
                                                         T  <-- Target position
                                                         
                                                         ^ 
              Clockwise circles with this center         |          Clockwise circles with this center will have
              will have > 180 deg of angular travel      |          < 180 deg of angular travel, which is a good thing!
                                               \         |          /   
  center of arc when h_x2_div_d is positive ->  x <----- | -----> x <- center of arc when h_x2_div_d is negative
                                                         |
                                                         |
                                                         
                                                         C  <-- Current position                                 */
                

        // Negative R is g-code-alese for "I want a circle with more than 180 degrees of travel" (go figure!), 
        // even though it is advised against ever generating such circles in a single line of g-code. By 
        // inverting the sign of h_x2_div_d the center of the circles is placed on the opposite side of the line of
        // travel and thus we get the unadvisably long arcs as prescribed.
        if (r < 0) { h_x2_div_d = -h_x2_div_d; }        
        // Complete the operation by calculating the actual center of the arc
        offset[gc.plane_axis_0] = (x-(y*h_x2_div_d))/2;
        offset[gc.plane_axis_1] = (y+(x*h_x2_div_d))/2;
      } 
      
      /*
         This segment sets up an clockwise or counterclockwise arc from the current position to the target position around 
         the center designated by the offset vector. All theta-values measured in radians of deviance from the positive 
         y-axis. 

                            | <- theta == 0
                          * * *                
                        *       *                                               
                      *           *                                             
                      *     O ----T   <- theta_end (e.g. 90 degrees: theta_end == PI/2)                                          
                      *   /                                                     
                        C   <- theta_start (e.g. -145 degrees: theta_start == -PI*(3/4))

      */
            
      // calculate the theta (angle) of the current point
      double theta_start = theta(-offset[gc.plane_axis_0], -offset[gc.plane_axis_1]);
      // calculate the theta (angle) of the target point
      double theta_end = theta(target[gc.plane_axis_0] - offset[gc.plane_axis_0] - gc.position[gc.plane_axis_0], 
         target[gc.plane_axis_1] - offset[gc.plane_axis_1] - gc.position[gc.plane_axis_1]);
      // double theta_end = theta(5,0);
      // ensure that the difference is positive so that we have clockwise travel
      if (theta_end < theta_start) { theta_end += 2*M_PI; }
      double angular_travel = theta_end-theta_start;
      // Invert angular motion if the g-code wanted a counterclockwise arc
      if (gc.motion_mode == MOTION_MODE_CCW_ARC) {
        angular_travel = angular_travel-2*M_PI;
      }
      // Find the radius
      double radius = hypot(offset[gc.plane_axis_0], offset[gc.plane_axis_1]);
      // Calculate the motion along the depth axis of the helix
      double depth = target[gc.plane_axis_2]-gc.position[gc.plane_axis_2];
      // Trace the arc
      mc_arc(theta_start, angular_travel, radius, depth, gc.plane_axis_0, gc.plane_axis_1, gc.plane_axis_2, 
        (gc.inverse_feed_rate_mode) ? inverse_feed_rate : gc.feed_rate, gc.inverse_feed_rate_mode);
      // Finish off with a line to make sure we arrive exactly where we think we are
      mc_line(target[X_AXIS], target[Y_AXIS], target[Z_AXIS], 
        (gc.inverse_feed_rate_mode) ? inverse_feed_rate : gc.feed_rate, gc.inverse_feed_rate_mode);
      break;
    }    
  }
  
  // As far as the parser is concerned, the position is now == target. In reality the
  // motion control system might still be processing the action and the real tool position
  // in any intermediate location.
  memcpy(gc.position, target, sizeof(double)*3);
  return(gc.status_code);
}

// Parses the next statement and leaves the counter on the first character following
// the statement. Returns 1 if there was a statements, 0 if end of string was reached
// or there was an error (check state.status_code).
int next_statement(char *letter, double *double_ptr, char *textline, int *counter) {
  if (textline[*counter] == 0) {
    return(0); // No more statements
  }
  
  *letter = textline[*counter];
  if((*letter < 'A') || (*letter > 'Z')) {
    FAIL(GCSTATUS_EXPECTED_COMMAND_LETTER);
    return(0);
  }
  (*counter)++;
  if (!read_double(textline, counter, double_ptr)) {
    return(0);
  };
  return(1);
}

int read_double(char *textline, 	//!< string: line of RS274/NGC code being processed
                int *counter,       //!< pointer to a counter for position on the line 
                double *double_ptr) //!< pointer to double to be read                  
{
  char *start = textline + *counter;
  char *end;
  
  *double_ptr = strtod(start, &end);
  if(end == start) { 
    FAIL(GCSTATUS_BAD_NUMBER_FORMAT); 
    return(0); 
  };

  *counter = end - textline;
  return(1);
}
