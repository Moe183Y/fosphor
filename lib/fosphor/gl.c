/*
 * gl.c
 *
 * OpenGL part of fosphor
 *
 * Copyright (C) 2013 Sylvain Munaut
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*! \addtogroup gl
 *  @{
 */

/*! \file gl.c
 *  \brief OpenGL part of fosphor
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gl_platform.h"

#include "axis.h"
#include "fosphor.h"
#include "gl.h"
#include "gl_cmap.h"
#include "gl_cmap_gen.h"
#include "gl_font.h"
#include "private.h"
#include "resource.h"


struct fosphor_gl_state
{
	int init_complete;

	struct gl_font *font;

	struct fosphor_gl_cmap_ctx *cmap_ctx;
	GLuint cmap_waterfall;
	GLuint cmap_histogram;

	GLuint tex_waterfall;
	GLuint tex_histogram;

	GLuint vbo_spectrum;
};


/* -------------------------------------------------------------------------- */
/* Helpers / Internal API                                                     */
/* -------------------------------------------------------------------------- */

static void
gl_tex2d_float_clear(GLuint tex_id, int width, int height)
{
	float buf[16*16];
	int x, y, cw, ch;

	memset(buf, 0x00, sizeof(buf));

	glBindTexture(GL_TEXTURE_2D, tex_id);

	for (y=0; y<height; y+=16) {
		for (x=0; x<width; x+=16) {
			cw = ((x+16) > width ) ? (width  - x) : 16;
			ch = ((y+16) > height) ? (height - y) : 16;
			glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, cw, ch, GL_RED, GL_FLOAT, buf);
		}
	}

}

static void
gl_vbo_clear(GLuint vbo_id, int size)
{
	void *ptr;

	glBindBuffer(GL_ARRAY_BUFFER, vbo_id);

	ptr = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
	if (!ptr)
		abort();

	memset(ptr, 0x00, size);

	glUnmapBuffer(GL_ARRAY_BUFFER);
}

#if 0
static void
gl_vbo_read(GLuint vbo_id, int size, void *dst)
{
	void *ptr;

	glBindBuffer(GL_ARRAY_BUFFER, vbo_id);

	ptr = glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
	if (!ptr)
		abort();

	memcpy(dst, ptr, size);

	glUnmapBuffer(GL_ARRAY_BUFFER);
}
#endif

static void
gl_deferred_init(struct fosphor *self)
{
	struct fosphor_gl_state *gl = self->gl;
	int len;

	/* Prevent double init */
	if (gl->init_complete)
		return;

	gl->init_complete = 1;

	/* Waterfall texture (FFT_LEN * 1024) */
	glGenTextures(1, &gl->tex_waterfall);

	glBindTexture(GL_TEXTURE_2D, gl->tex_waterfall);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, FOSPHOR_FFT_LEN, 1024, 0, GL_RED, GL_FLOAT, NULL);

	gl_tex2d_float_clear(gl->tex_waterfall, FOSPHOR_FFT_LEN, 1024);

	/* Histogram texture (FFT_LEN * 128) */
	glGenTextures(1, &gl->tex_histogram);

	glBindTexture(GL_TEXTURE_2D, gl->tex_histogram);

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, FOSPHOR_FFT_LEN, 128, 0, GL_RED, GL_FLOAT, NULL);

	gl_tex2d_float_clear(gl->tex_histogram, FOSPHOR_FFT_LEN, 128);

	/* Spectrum VBO (2 * FFT_LEN, half for live, half for 'hold') */
	glGenBuffers(1, &gl->vbo_spectrum);

	glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_spectrum);

	len = 2 * sizeof(float) * 2 * FOSPHOR_FFT_LEN;
	glBufferData(GL_ARRAY_BUFFER, len, NULL, GL_DYNAMIC_DRAW);

	gl_vbo_clear(gl->vbo_spectrum, len);
}


/* -------------------------------------------------------------------------- */
/* Exposed API                                                                */
/* -------------------------------------------------------------------------- */

int
fosphor_gl_init(struct fosphor *self)
{
	struct fosphor_gl_state *gl;
	const void *font_data;
	int len, rv;

	/* Allocate structure */
	gl = malloc(sizeof(struct fosphor_gl_state));
	if (!gl)
		return -ENOMEM;

	self->gl = gl;

	memset(gl, 0, sizeof(struct fosphor_gl_state));

	/* Font */
	gl->font = glf_alloc(8, GLF_FLG_LCD);
	if (!gl->font) {
		rv = -ENOMEM;
		goto error;
	}

	font_data = resource_get("DroidSansMonoDotted.ttf", &len);
	if (!font_data) {
		rv = -ENOENT;
		goto error;
	}

	rv = glf_load_face_mem(gl->font, font_data, len);
	if (rv)
		goto error;

	/* Color mapping */
	gl->cmap_ctx = fosphor_gl_cmap_init();

	rv  = (gl->cmap_ctx == NULL);

	rv |= fosphor_gl_cmap_generate(&gl->cmap_waterfall,
	                               fosphor_gl_cmap_waterfall, 256);
	rv |= fosphor_gl_cmap_generate(&gl->cmap_histogram,
	                               fosphor_gl_cmap_histogram, 256);

	if (rv)
		goto error;

	/* Done */
	return 0;

error:
	fosphor_gl_release(self);

	return rv;
}

void
fosphor_gl_release(struct fosphor *self)
{
	struct fosphor_gl_state *gl = self->gl;

	/* Safety */
	if (!gl)
		return;

	/* Release all */
	glDeleteBuffers(1, &gl->vbo_spectrum);

	glDeleteTextures(1, &gl->tex_histogram);
	glDeleteTextures(1, &gl->tex_waterfall);

	glDeleteTextures(1, &gl->cmap_histogram);
	glDeleteTextures(1, &gl->cmap_waterfall);
	fosphor_gl_cmap_release(gl->cmap_ctx);

	glf_free(gl->font);

	/* Release structure */
	free(gl);

	/* Nothing left */
	self->gl = NULL;
}


GLuint
fosphor_gl_get_shared_id(struct fosphor *self,
                         enum fosphor_gl_id id)
{
	struct fosphor_gl_state *gl = self->gl;

	/* CL is not sufficiently booted to complete the GL init
	 * in a CL context */
	gl_deferred_init(self);

	/* Select ID to return */
	switch (id) {
	case GL_ID_TEX_WATERFALL:
		return gl->tex_waterfall;

	case GL_ID_TEX_HISTOGRAM:
		return gl->tex_histogram;

	case GL_ID_VBO_SPECTRUM:
		return gl->vbo_spectrum;
	}

	return 0;
}


void
fosphor_gl_draw(struct fosphor *self, struct fosphor_render *render)
{
	struct fosphor_gl_state *gl = self->gl;
	struct freq_axis freq_axis;
	float x[2], y[2], u[2], v[2];
	float tw, bw;
	int i;

	/* Utils */
	tw = 1.0f / (float)FOSPHOR_FFT_LEN;	/* Texel width */
	bw = 1.0f / (float)(FOSPHOR_FFT_LEN-1);	/* Bin width (displayed) */

	/* Texture mapping notes:
	 *
	 *  - The texture have the "DC" bin at texel 0, however we want it to
	 *    be displayed centered and to do so, texture coordinates are used.
	 *  - One of the bin is not displayed (the one at u=0.5f) because
	 *    it is neither positive freq, nor negative ones, but both. To
	 *    compensate for this, the (1.0f - tw) factor is used.
	 *
	 * Vertex mapping notes:
	 *
	 *  - We want the vertex to appear at the center of the displayed bins
	 *  - The vertex 'X' coordinates are filled in by the display kernel as
	 *    ((bin #) ^ (N >> 1)) / (N >> 1) - 1
	 *  - So the DC bin is 0.0f and the undisplayed bin is -1. The others
	 *    are spread between [ -1+2*tw  to  1-2*tw ]
	 *  - For display, that range is first remapped to [0 to 1], then to
	 *    [ bw/2 to 1-bw/2 ] (where bw is normalized displayed bin width)
	 *    so that each point maps to the center of the bin on the textures.
	 *  - Finally the zoom is applied and then the transform to map on the
	 *    requested screen area
	 */

        /* Draw waterfall */
	if (render->options & FRO_WATERFALL)
	{
		x[0] = render->_x[0];
		x[1] = render->_x[1];

		y[0] = render->_y_wf[0];
		y[1] = render->_y_wf[1];

		u[0] = 0.5f + tw + ((1.0f - tw) * render->freq_start);
		u[1] = 0.5f + tw + ((1.0f - tw) * render->freq_stop);

		v[1] = (float)render->_wf_pos / 1024.0f;
		v[0] = v[1] - render->wf_span;

		fosphor_gl_cmap_enable(gl->cmap_ctx,
		                       gl->tex_waterfall, gl->cmap_waterfall,
		                       self->power.scale, self->power.offset,
		                       GL_CMAP_MODE_BILINEAR);

		glBegin( GL_QUADS );
		glTexCoord2f(u[0], v[0]); glVertex2f(x[0], y[0]);
		glTexCoord2f(u[1], v[0]); glVertex2f(x[1], y[0]);
		glTexCoord2f(u[1], v[1]); glVertex2f(x[1], y[1]);
		glTexCoord2f(u[0], v[1]); glVertex2f(x[0], y[1]);
		glEnd();

		fosphor_gl_cmap_disable();
	}

	/* Draw histogram */
	if (render->options & FRO_HISTO)
	{
		x[0] = render->_x[0];
		x[1] = render->_x[1];

		y[0] = render->_y_histo[0];
		y[1] = render->_y_histo[1];

		u[0] = 0.5f + tw + ((1.0f - tw) * render->freq_start);
		u[1] = 0.5f + tw + ((1.0f - tw) * render->freq_stop);

		v[0] = 0.0f;
		v[1] = 1.0f;

		fosphor_gl_cmap_enable(gl->cmap_ctx,
		                       gl->tex_histogram, gl->cmap_histogram,
		                       1.1f, 0.0f, GL_CMAP_MODE_BILINEAR);

		glBegin( GL_QUADS );
		glTexCoord2f(u[0], v[0]); glVertex2f(x[0], y[0]);
		glTexCoord2f(u[1], v[0]); glVertex2f(x[1], y[0]);
		glTexCoord2f(u[1], v[1]); glVertex2f(x[1], y[1]);
		glTexCoord2f(u[0], v[1]); glVertex2f(x[0], y[1]);
		glEnd();

		fosphor_gl_cmap_disable();
	}
	else if (render->options & (FRO_LIVE | FRO_MAX_HOLD))
	{
		x[0] = render->_x[0];
		x[1] = render->_x[1];

		y[0] = render->_y_histo[0];
		y[1] = render->_y_histo[1];

		glColor3f(0.0f, 0.0f, 0.1f);

		glBegin( GL_QUADS );
		glVertex2f(x[0], y[0]);
		glVertex2f(x[1], y[0]);
		glVertex2f(x[1], y[1]);
		glVertex2f(x[0], y[1]);
		glEnd();
	}

	/* Draw spectrum */
	if (render->options & (FRO_LIVE | FRO_MAX_HOLD))
	{
		int idx[2], len;

		/* Select end-points */
		idx[0] = 1 + (int)ceilf (render->freq_start * (float)(FOSPHOR_FFT_LEN - 1) - 0.5f);
		idx[1] = 1 + (int)floorf(render->freq_stop  * (float)(FOSPHOR_FFT_LEN - 1) - 0.5f);
		len = idx[1] - idx[0] + 1;

		/* Setup */
		glPushMatrix();

			/* Screen position scaling */
		glTranslatef(
			render->_x[0],
			render->_y_histo[0],
			0.0f
		);

		glScalef(
			render->_x[1] - render->_x[0],
			render->_y_histo[1] - render->_y_histo[0],
			1.0f
		);

			/* Power offset / scaling */
		glScalef(1.0f, self->power.scale, 1.0f);
		glTranslatef(0.0f, self->power.offset, 0.0f);

			/* Spectrum range selection */
		glScalef(1.0f / (render->freq_stop - render->freq_start), 1.0f, 1.0f);
		glTranslatef(-render->freq_start, 0.0f, 0.0f);

			/* Map the center of each N-1 bins */
		glTranslatef(0.5f * bw, 0.0f, 0.0f);
		glScalef(1.0f - bw, 1.0f, 1.0f);

			/* Spectrum x scaling to [0.0 -> 1.0] range */
		glTranslatef(0.5f, 0.0f, 0.0f);
		glScalef(0.5f / (1.0f - 2.0f * tw), 1.0f, 1.0f);

			/* GL state setup */
		glBindBuffer(GL_ARRAY_BUFFER, gl->vbo_spectrum);
		glVertexPointer(2, GL_FLOAT, 0, 0);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glEnable(GL_LINE_SMOOTH);
		glLineWidth(1.0f);

		/* Live */
		if (render->options & FRO_LIVE)
		{
			glColor4f(1.0f, 1.0f, 1.0f, 0.75f);

			glEnableClientState(GL_VERTEX_ARRAY);
			glDrawArrays(GL_LINE_STRIP, idx[0], len);
			glDisableClientState(GL_VERTEX_ARRAY);
		}

		/* Max hold */
		if (render->options & FRO_MAX_HOLD)
		{
			glColor4f(1.0f, 0.0f, 0.0f, 0.75f);

			glEnableClientState(GL_VERTEX_ARRAY);
			glDrawArrays(GL_LINE_STRIP, idx[0] + FOSPHOR_FFT_LEN, len);
			glDisableClientState(GL_VERTEX_ARRAY);
		}

		/* Cleanup */
		glDisable(GL_BLEND);

		glPopMatrix();
	}

	/* Setup frequency axis */
	freq_axis_build(&freq_axis,
	                self->frequency.center,
	                self->frequency.span
	);

	/* Draw grid */
	if (render->options & (FRO_LIVE | FRO_MAX_HOLD | FRO_HISTO))
	{
		for (i=0; i<11; i++)
		{
			float fg_color[3] = { 1.00f, 1.00f, 0.33f };
			float xv, yv, xv_ofs;
			char buf[32];

			xv = render->_x[0]       + i * render->_x_div;
			yv = render->_y_histo[0] + i * render->_y_histo_div;

			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glColor4f(0.0f, 0.0f, 0.0f, 0.5f);

			glBegin(GL_LINES);
			glVertex2f(xv + 0.5f, render->_y_histo[0]+0.5f);
			glVertex2f(xv + 0.5f, render->_y_histo[1]-0.5f);
			glEnd();

			glBegin(GL_LINES);
			glVertex2f(render->_x[0] + 0.5f, yv + 0.5f);
			glVertex2f(render->_x[1] - 0.5f, yv + 0.5f);
			glEnd();

			glDisable(GL_BLEND);

			glf_begin(gl->font, fg_color);

			if (render->options & FRO_LABEL_PWR) {
				glf_printf(gl->font,
				           render->_x_label, GLF_RIGHT,
				           yv, GLF_CENTER,
				           "%d", self->power.db_ref - (10-i) * self->power.db_per_div
				);
			}

			if (render->options & FRO_LABEL_FREQ) {
				freq_axis_render(&freq_axis, buf, i-5);

				xv_ofs = (i == 0) ? 5.0f : (i == 10 ? -5.0f : 0.0f);

				glf_printf(gl->font,
				           xv + xv_ofs, GLF_CENTER,
				           render->_y_label, GLF_CENTER,
				           "%s", buf
				);
			}

			glf_end();
		}
	}

	/* Ensure GL is done */
	glFinish();
}

/*! @} */
