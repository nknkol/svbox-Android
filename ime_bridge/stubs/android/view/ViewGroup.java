/* SPDX-License-Identifier: MIT */
package android.view;

public class ViewGroup {
    public static class LayoutParams {
        public static final int MATCH_PARENT = -1;
        public int width;
        public int height;
        public LayoutParams(int width, int height) {
            this.width = width;
            this.height = height;
        }
    }
}
