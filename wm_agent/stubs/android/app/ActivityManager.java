package android.app;

import android.content.ComponentName;

public class ActivityManager {
    public static class RunningTaskInfo {
        public int taskId;
        public int displayId;
        public ComponentName topActivity;
        public ComponentName baseActivity;
        public int numActivities;
    }
}
