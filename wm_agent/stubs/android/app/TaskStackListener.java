package android.app;

import android.content.ComponentName;

public abstract class TaskStackListener {
    public void onTaskCreated(int taskId, ComponentName componentName) {}
    public void onTaskMovedToFront(ActivityManager.RunningTaskInfo taskInfo) {}
    public void onTaskRemovalStarted(ActivityManager.RunningTaskInfo taskInfo) {}
    public void onTaskDisplayChanged(int taskId, int newDisplayId) {}
}
