/* SPDX-License-Identifier: MIT */
package android.net;

public class LocalSocketAddress {
    public enum Namespace {
        ABSTRACT,
        RESERVED,
        FILESYSTEM
    }
    public LocalSocketAddress(String name, Namespace namespace) {}
}
