/*
 * SPDX-License-Identifier: MIT
 */

#ifndef COG_CURSOR_DRM_H
#define COG_CURSOR_DRM_H

/* cursor pixmap patterns */
extern const char *cursor_pointer;
extern const char *cursor_hand;

/* cursor functions */
int init_cursor(int drm_fd, int crtc_id);
void set_cursor(const char *pattern);
void move_cursor(int drm_fd, int crtc_id, int x, int y);
void clear_cursor(int drm_fd, int crtc_id);

#endif //COG_CURSOR_DRM_H
