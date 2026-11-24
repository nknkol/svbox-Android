package android.hardware.display;

public interface IVirtualDisplayCallback {
    abstract class Stub implements IVirtualDisplayCallback {
        public void onPaused() {}
        public void onResumed() {}
        public void onStopped() {}
    }
}
