import { useConnectionKey, useRequestLifetime } from "../requestLifetime";
import { errorKind } from "../logPrivacy";
/**
 * ProfileManager Component
 *
 * Displays and manages BLE profiles for the connected keyboard.
 */

import { useContext, useState, useEffect, useCallback } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import { SUBSYSTEM_IDENTIFIER } from "../App";
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";
import {
  Request,
  Response,
  ProfileInfo,
  PeerIdentity,
} from "../proto/zmk/ble_management/ble_management";
import "./ProfileManager.css";

export function ProfileManager({
  refreshRevision = 0,
}: {
  refreshRevision?: number;
}) {
  const app = useContext(ZMKAppContext);
  const index = app?.findSubsystem(SUBSYSTEM_IDENTIFIER)?.index;
  const key = useConnectionKey(app?.state.connection, index);
  return <ProfileManagerState key={`${key}:${refreshRevision}`} />;
}

function ProfileManagerState() {
  const zmkApp = useContext(ZMKAppContext);
  const beginRequest = useRequestLifetime();
  const [profiles, setProfiles] = useState<ProfileInfo[]>([]);
  const [maxProfiles, setMaxProfiles] = useState<number>(0);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [editName, setEditName] = useState("");
  const [editPeer, setEditPeer] = useState<PeerIdentity | undefined>();

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);

  // Load profiles on mount and when subsystem changes
  const loadProfiles = useCallback(
    async () => {
      if (!zmkApp?.state.connection || !subsystem) return;

      const active = beginRequest();
      if (!active()) return;
      setIsLoading(true);
      setError(null);

      try {
        const service = new ZMKCustomSubsystem(
          zmkApp.state.connection,
          subsystem.index
        );

        const request = Request.create({
          getProfiles: {},
        });

        const payload = Request.encode(request).finish();
        const responsePayload = await service.callRPC(payload);
        if (!active()) return;

        if (responsePayload) {
          const resp = Response.decode(responsePayload);
          if (resp.getProfiles) {
            setProfiles(resp.getProfiles.profiles);
            setMaxProfiles(resp.getProfiles.maxProfiles);
          } else if (resp.error) {
            setError(resp.error.message);
          }
        }
      } catch (err) {
        if (!active()) return;
        console.error("Failed to load profiles:", errorKind(err));
        setError(
          `Failed to load profiles: ${err instanceof Error ? err.message : "Unknown error"}`
        );
      } finally {
        if (active()) setIsLoading(false);
      }
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [zmkApp?.state.connection, subsystem?.index, beginRequest]
  );

  useEffect(() => {
    if (subsystem && zmkApp?.state.connection) {
      loadProfiles();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index, zmkApp?.state.connection, loadProfiles]);

  useEffect(() => {
    if (!zmkApp?.state.connection || !subsystem) return;
    let current = true;
    const unsubscribe = zmkApp.onNotification({
      type: "core",
      callback: (notification) => {
        if (!current || notification.lockStateChanged === undefined) return;
        // Retire pending results synchronously, before React renders redacted
        // state. A reply admitted before relocking must not restore addresses.
        beginRequest();
        if (
          notification.lockStateChanged ===
          LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED
        ) {
          void loadProfiles();
          return;
        }
        setProfiles((previous) =>
          previous.map((profile) => ({
            ...profile,
            address: "",
            peerIdentity: undefined,
          }))
        );
        setEditingIndex(null);
        setEditName("");
        setEditPeer(undefined);
        setIsLoading(false);
        setError(null);
      },
    });
    return () => {
      current = false;
      unsubscribe();
    };
  }, [zmkApp, subsystem, beginRequest, loadProfiles]);

  const switchProfile = async (index: number) => {
    if (!zmkApp?.state.connection || !subsystem) return;

    const active = beginRequest();
    if (!active()) return;
    setIsLoading(true);
    setError(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      const request = Request.create({
        switchProfile: { index },
      });

      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!active()) return;

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        if (resp.switchProfile?.success) {
          // Reload profiles to update active status
          await loadProfiles();
        } else if (resp.error) {
          setError(resp.error.message);
        } else {
          setError("Failed to switch profile");
        }
      }
    } catch (err) {
      if (!active()) return;
      console.error("Failed to switch profile:", errorKind(err));
      setError(
        `Failed to switch profile: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      if (active()) setIsLoading(false);
    }
  };

  const unpairProfile = async (profile: ProfileInfo) => {
    if (!zmkApp?.state.connection || !subsystem) return;
    if (!profile.peerIdentity) {
      setError("Unlock Studio and refresh profiles before changing a peer");
      return;
    }
    if (!confirm("Are you sure you want to unpair this device?")) return;

    const active = beginRequest();
    if (!active()) return;
    setIsLoading(true);
    setError(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      const request = Request.create({
        unpairProfile: {
          index: profile.index,
          expectedPeer: profile.peerIdentity,
        },
      });

      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!active()) return;

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        if (resp.unpairProfile?.success) {
          await loadProfiles();
        } else if (resp.error) {
          setError(resp.error.message);
        } else {
          setError("Failed to unpair profile");
        }
      }
    } catch (err) {
      if (!active()) return;
      console.error("Failed to unpair profile:", errorKind(err));
      setError(
        `Failed to unpair profile: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      if (active()) setIsLoading(false);
    }
  };

  const saveProfileName = async (index: number, name: string) => {
    if (!zmkApp?.state.connection || !subsystem) return;
    if (!editPeer) {
      setError("Unlock Studio and refresh profiles before changing a peer");
      return;
    }
    if (new TextEncoder().encode(name).length > 31) {
      setError("Names must fit within 31 UTF-8 bytes");
      return;
    }

    const active = beginRequest();
    if (!active()) return;
    setIsLoading(true);
    setError(null);

    try {
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      const request = Request.create({
        setProfileName: { index, name, expectedPeer: editPeer },
      });

      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!active()) return;

      if (responsePayload) {
        const resp = Response.decode(responsePayload);
        if (resp.setProfileName?.success) {
          setEditingIndex(null);
          setEditName("");
          setEditPeer(undefined);
          await loadProfiles();
        } else if (resp.error) {
          setError(resp.error.message);
        } else {
          setError("Failed to save profile name");
        }
      }
    } catch (err) {
      if (!active()) return;
      console.error("Failed to save profile name:", errorKind(err));
      setError(
        `Failed to save profile name: ${err instanceof Error ? err.message : "Unknown error"}`
      );
    } finally {
      if (active()) setIsLoading(false);
    }
  };

  const startEditing = (profile: ProfileInfo) => {
    if (!profile.peerIdentity) {
      setError("Unlock Studio and refresh profiles before changing a peer");
      return;
    }
    setEditingIndex(profile.index);
    setEditName(profile.name);
    setEditPeer(profile.peerIdentity);
  };

  const cancelEditing = () => {
    setEditingIndex(null);
    setEditName("");
    setEditPeer(undefined);
  };

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ BLE Management subsystem not found. Make sure your firmware
            includes the BLE management module.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card profile-manager">
      <h2>📱 Bluetooth Profiles</h2>
      <p>
        Manage up to {maxProfiles} paired devices. Click on a profile to switch
        connections.
      </p>

      {error && (
        <div className="error-message">
          <p>🚨 {error}</p>
        </div>
      )}

      {isLoading && profiles.length === 0 && <p>⏳ Loading profiles...</p>}

      <div className="profiles-list">
        {profiles.map((profile) => (
          <div
            key={profile.index}
            className={`profile-item ${profile.isActive ? "active" : ""} ${
              profile.isConnected ? "connected" : ""
            } ${profile.isOpen ? "empty" : ""}`}
          >
            <div className="profile-header">
              <h3>
                Profile {profile.index + 1}
                {profile.isActive && " ⭐"}
                {profile.isConnected && " 🔗"}
              </h3>
              {profile.isActive && <span className="badge active">Active</span>}
              {profile.isConnected && !profile.isActive && (
                <span className="badge connected">Connected</span>
              )}
              {profile.isOpen && <span className="badge empty">Empty</span>}
            </div>

            {!profile.isOpen ? (
              <>
                <div className="profile-info">
                  {editingIndex === profile.index ? (
                    <div className="edit-name">
                      <input
                        type="text"
                        value={editName}
                        disabled={isLoading}
                        onChange={(e) => setEditName(e.target.value)}
                        placeholder="Device name"
                        maxLength={31}
                      />
                      <button
                        className="btn btn-sm btn-primary"
                        onClick={() => saveProfileName(profile.index, editName)}
                        disabled={isLoading}
                      >
                        ✓ Save
                      </button>
                      <button
                        className="btn btn-sm btn-secondary"
                        onClick={cancelEditing}
                        disabled={isLoading}
                      >
                        ✗ Cancel
                      </button>
                    </div>
                  ) : (
                    <>
                      <p className="profile-name">
                        <strong>Name:</strong>{" "}
                        {profile.name || (
                          <em className="text-muted">Not named</em>
                        )}
                        <button
                          className="btn btn-link"
                          onClick={() => startEditing(profile)}
                          disabled={isLoading}
                          title="Edit name"
                        >
                          ✏️
                        </button>
                      </p>
                      <p className="profile-address">
                        <strong>Address:</strong>{" "}
                        <code>{profile.address || "N/A"}</code>
                      </p>
                    </>
                  )}
                </div>

                <div className="profile-actions">
                  {!profile.isActive && (
                    <button
                      className="btn btn-primary"
                      onClick={() => switchProfile(profile.index)}
                      disabled={isLoading}
                    >
                      Switch to this profile
                    </button>
                  )}
                  <button
                    className="btn btn-danger"
                    onClick={() => unpairProfile(profile)}
                    disabled={isLoading}
                  >
                    🗑️ Unpair
                  </button>
                </div>
              </>
            ) : (
              <>
                <p className="text-muted">No device paired in this slot.</p>
                <div className="profile-actions">
                  {!profile.isActive && (
                    <button
                      className="btn btn-primary"
                      onClick={() => switchProfile(profile.index)}
                      disabled={isLoading}
                    >
                      Switch to this profile
                    </button>
                  )}
                </div>
              </>
            )}
          </div>
        ))}
      </div>

      <button
        className="btn btn-secondary"
        onClick={loadProfiles}
        disabled={isLoading}
      >
        🔄 Refresh
      </button>
    </section>
  );
}
