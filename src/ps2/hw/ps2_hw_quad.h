// OPT10 (HT, PS2-HW-72): can a primitive put a pixel on the screen? Pure code, also compiled by the host test tools/ps2/hw_quad_hosttest.py.
//
// The GS draws a pixel when its centre lies inside the primitive. In the driver's coordinates (see P.ox in ps2_hw_draw.inc: the -0.5 shift) the centres of
// the pixels are the integer points. A sprite quad whose screen box (x0..x1, y0..y1 in those coordinates) holds no integer point in x or none in y holds
// no pixel centre and draws nothing. The GS snaps the vertices to 1/16 pixel (12.4 fixed point): the box is widened by QUAD_MARGIN so that the
// snapped quad cannot reach a centre the test did not see.

#ifndef __PS2_HW_QUAD_H__
#define __PS2_HW_QUAD_H__

#include <math.h>

#define QUAD_MARGIN 0.1f // pixels; the snapping moves a vertex by less than 1/16

// 1 = the box cannot contain a pixel centre (the primitive draws nothing)
static inline int quad_no_centre(float x0, float x1, float y0, float y1)
{
	return ceilf(x0 - QUAD_MARGIN) > floorf(x1 + QUAD_MARGIN) || ceilf(y0 - QUAD_MARGIN) > floorf(y1 + QUAD_MARGIN);
}

#endif
