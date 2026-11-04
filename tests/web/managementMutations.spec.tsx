import type { ReactNode } from "react";
import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
  within,
} from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { createConnectedMockZMKApp } from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import App, { SUBSYSTEM_IDENTIFIER } from "../../web/src/App";
import {
  Request,
  Response,
} from "../../web/src/proto/zmk/ble_management/ble_management";

let mockConnectedApp: ReturnType<typeof createConnectedMockZMKApp>;
// The connection control is a boundary; App's panel wiring, the provider,
// service wrapper and generated owner codec remain the actual implementations.
jest.mock("@cormoran/zmk-studio-react-hook", () => {
  const actual = jest.requireActual("@cormoran/zmk-studio-react-hook");
  const React = jest.requireActual("react");
  return {
    ...actual,
    ZMKConnection: ({
      renderConnected,
    }: {
      renderConnected: (props: {
        disconnect: () => void;
        deviceName: string;
      }) => ReactNode;
    }) =>
      React.createElement(
        jest.requireActual("@cormoran/zmk-studio-react-hook/testing")
          .ZMKAppProvider,
        { value: mockConnectedApp },
        renderConnected({
          disconnect: jest.fn(),
          deviceName: "Fixture device",
        }),
      ),
  };
});
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({ call_rpc: jest.fn() }));
jest.mock("@zmkfirmware/zmk-studio-ts-client/transport/serial", () => ({
  connect: jest.fn(),
}));

function wire(value: Parameters<typeof Response.create>[0]) {
  return {
    custom: {
      call: { payload: Response.encode(Response.create(value)).finish() },
    },
  };
}
let forgotten: boolean, name: string, profileReads: number, splitReads: number;
let intercept: ((request: Request) => unknown) | undefined;
function profiles() {
  return wire({
    getProfiles: {
      maxProfiles: 5,
      profiles: Array.from({ length: 5 }, (_, index) => ({
        index,
        name: index === 1 && !forgotten ? name : "",
        address: index === 1 && !forgotten ? "01:02:03:04:05:06" : "",
        peerIdentity:
          index === 1 && !forgotten
            ? { addressType: 0, address: new Uint8Array([6, 5, 4, 3, 2, 1]) }
            : undefined,
        isOpen: index !== 1 || forgotten,
        isActive: index === (forgotten ? 0 : 1),
        isConnected: false,
      })),
    },
  });
}
beforeEach(() => {
  jest.clearAllMocks();
  mockConnectedApp = createConnectedMockZMKApp({
    subsystems: [SUBSYSTEM_IDENTIFIER],
  });
  forgotten = false;
  name = "fixture-host";
  profileReads = splitReads = 0;
  intercept = undefined;
  jest.spyOn(window, "confirm").mockReturnValue(true);
  jest.spyOn(window, "alert").mockImplementation(() => {});
  (call_rpc as jest.Mock).mockImplementation((_connection, raw) => {
    const request = Request.decode(raw.custom.call.payload);
    const blocked = intercept?.(request);
    if (blocked !== undefined) return blocked;
    if (request.getProfiles) {
      profileReads++;
      return Promise.resolve(profiles());
    }
    if (request.getSplitInfo) {
      splitReads++;
      return Promise.resolve(
        wire({
          getSplitInfo: {
            info: { isSplit: true, isCentral: true, peripheralConnected: true },
          },
        }),
      );
    }
    if (request.getOutputPriority)
      return Promise.resolve(wire({ getOutputPriority: { priority: 0 } }));
    if (request.forgetSplitBond) {
      forgotten = true;
      return Promise.resolve(wire({ forgetSplitBond: { success: true } }));
    }
    if (request.setProfileName) {
      name = request.setProfileName.name;
      return Promise.resolve(wire({ setProfileName: { success: true } }));
    }
    throw new Error("Unexpected fixture request");
  });
});
afterEach(() => jest.restoreAllMocks());

it("refreshes host profiles after clearing host pairings from the split panel", async () => {
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(
    screen.getByRole("button", { name: /Clear All Host Pairings/ }),
  );
  await waitFor(() => expect(splitReads).toBe(2));
  await waitFor(() =>
    expect(screen.queryByText("fixture-host")).not.toBeInTheDocument(),
  );
  expect(profileReads).toBe(2);
  expect(screen.getAllByText("No device paired in this slot.")).toHaveLength(5);
});

it("keeps profiles when clearing is refused and permits a later successful retry", async () => {
  let refuse = true;
  intercept = (request) =>
    request.forgetSplitBond && refuse
      ? Promise.resolve(wire({ error: { message: "Fixture refusal" } }))
      : undefined;
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(
    screen.getByRole("button", { name: /Clear All Host Pairings/ }),
  );
  await screen.findByText(/Fixture refusal/);
  expect(profileReads).toBe(1);
  expect(screen.getByText("fixture-host")).toBeInTheDocument();
  refuse = false;
  fireEvent.click(
    screen.getByRole("button", { name: /Clear All Host Pairings/ }),
  );
  await waitFor(() => expect(profileReads).toBe(2));
  expect(screen.queryByText("fixture-host")).not.toBeInTheDocument();
});

it("does not refresh a replacement connection after an old clear finishes", async () => {
  let finish!: (reply: ReturnType<typeof wire>) => void;
  intercept = (request) =>
    request.forgetSplitBond
      ? new Promise<ReturnType<typeof wire>>((resolve) => {
          finish = resolve;
        })
      : undefined;
  const result = render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(
    screen.getByRole("button", { name: /Clear All Host Pairings/ }),
  );
  await waitFor(() => expect(finish).toBeDefined());
  mockConnectedApp = createConnectedMockZMKApp({
    subsystems: [SUBSYSTEM_IDENTIFIER],
  });
  name = "current-host";
  result.rerender(<App />);
  await screen.findByText("current-host");
  expect(profileReads).toBe(2);
  await act(async () => {
    finish(wire({ forgetSplitBond: { success: true } }));
  });
  expect(profileReads).toBe(2);
  expect(screen.getByText("current-host")).toBeInTheDocument();
  expect(window.alert).not.toHaveBeenCalled();
});

it("retires an old name draft when clearing pairings replaces the profiles", async () => {
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.change(screen.getByPlaceholderText("Device name"), {
    target: { value: "old-draft" },
  });
  fireEvent.click(
    screen.getByRole("button", { name: /Clear All Host Pairings/ }),
  );
  await waitFor(() => expect(profileReads).toBe(2));
  forgotten = false;
  name = "replacement-host";
  const panel = document.querySelector(".profile-manager") as HTMLElement;
  fireEvent.click(within(panel).getByRole("button", { name: /Refresh/ }));
  await screen.findByText("replacement-host");
  expect(screen.queryByPlaceholderText("Device name")).not.toBeInTheDocument();
  expect(screen.getByTitle("Edit name")).toBeEnabled();
});

it("does not keep old profile cards when the post-clear read fails", async () => {
  intercept = (request) =>
    request.getProfiles && forgotten
      ? Promise.reject(new Error("Fixture refresh failure"))
      : undefined;
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(
    screen.getByRole("button", { name: /Clear All Host Pairings/ }),
  );
  await screen.findByText(/Fixture refresh failure/);
  expect(screen.queryByText("fixture-host")).not.toBeInTheDocument();
  expect(screen.queryByTitle("Edit name")).not.toBeInTheDocument();
});

it("prevents entering another name while a save is pending", async () => {
  let finish!: (reply: ReturnType<typeof wire>) => void;
  let sent = "";
  intercept = (request) => {
    if (!request.setProfileName) return undefined;
    sent = request.setProfileName.name;
    return new Promise<ReturnType<typeof wire>>((resolve) => {
      finish = resolve;
    });
  };
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.change(screen.getByPlaceholderText("Device name"), {
    target: { value: "saved-label" },
  });
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await waitFor(() => expect(sent).toBe("saved-label"));
  const input = screen.getByPlaceholderText("Device name");
  expect(input).toBeDisabled();
  await userEvent.type(input, "newer-draft");
  expect(input).toHaveValue("saved-label");
  await act(async () => {
    name = sent;
    finish(wire({ setProfileName: { success: true } }));
  });
  await screen.findByText("saved-label");
  expect(profileReads).toBe(2);
  expect(screen.queryByPlaceholderText("Device name")).not.toBeInTheDocument();
});

it("restores an unchanged editable name after a save rejection", async () => {
  let reject!: (error: Error) => void;
  intercept = (request) =>
    request.setProfileName
      ? new Promise((_resolve, fail) => {
          reject = fail;
        })
      : undefined;
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.change(screen.getByPlaceholderText("Device name"), {
    target: { value: "retry-label" },
  });
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await waitFor(() => expect(reject).toBeDefined());
  expect(screen.getByPlaceholderText("Device name")).toBeDisabled();
  await act(async () => {
    reject(new Error("Fixture transport error"));
  });
  expect(screen.getByPlaceholderText("Device name")).toBeEnabled();
  expect(screen.getByPlaceholderText("Device name")).toHaveValue("retry-label");
  expect(profileReads).toBe(1);
  const panel = document.querySelector(".profile-manager") as HTMLElement;
  expect(within(panel).getByRole("button", { name: /Save/ })).toBeEnabled();
});

it.each(["success", "error"])(
  "retires a pending pre-clear profile read (%s)",
  async (outcome) => {
    render(<App />);
    await screen.findByText("fixture-host");
    let finish!: (value: ReturnType<typeof wire>) => void;
    let fail!: (error: Error) => void;
    const stale = profiles();
    let blocked = false;
    intercept = (request) => {
      if (!request.getProfiles || forgotten || blocked) return undefined;
      blocked = true;
      profileReads++;
      return new Promise<ReturnType<typeof wire>>((resolve, reject) => {
        finish = resolve;
        fail = reject;
      });
    };
    const panel = document.querySelector(".profile-manager") as HTMLElement;
    fireEvent.click(within(panel).getByRole("button", { name: /Refresh/ }));
    await waitFor(() => expect(finish).toBeDefined());
    fireEvent.click(
      screen.getByRole("button", { name: /Clear All Host Pairings/ }),
    );
    await waitFor(() => expect(profileReads).toBe(3));
    expect(screen.getAllByText("No device paired in this slot.")).toHaveLength(
      5,
    );
    await act(async () => {
      if (outcome === "success") finish(stale);
      else fail(new Error("Retired pre-clear read"));
    });
    expect(screen.queryByText("fixture-host")).not.toBeInTheDocument();
    expect(
      screen.queryByText(/Retired pre-clear read/),
    ).not.toBeInTheDocument();
    const current = document.querySelector(".profile-manager") as HTMLElement;
    expect(
      within(current).getByRole("button", { name: /Refresh/ }),
    ).toBeEnabled();
  },
);

it.each(["success", "error"])(
  "retires a pending name save after clear (%s)",
  async (outcome) => {
    let finish!: (value: ReturnType<typeof wire>) => void;
    let fail!: (error: Error) => void;
    intercept = (request) =>
      request.setProfileName
        ? new Promise<ReturnType<typeof wire>>((resolve, reject) => {
            finish = resolve;
            fail = reject;
          })
        : undefined;
    render(<App />);
    await screen.findByText("fixture-host");
    fireEvent.click(screen.getByTitle("Edit name"));
    fireEvent.change(screen.getByPlaceholderText("Device name"), {
      target: { value: "retired-name" },
    });
    fireEvent.click(screen.getByRole("button", { name: /Save/ }));
    await waitFor(() => expect(finish).toBeDefined());
    expect(screen.getByPlaceholderText("Device name")).toBeDisabled();
    fireEvent.click(
      screen.getByRole("button", { name: /Clear All Host Pairings/ }),
    );
    await waitFor(() => expect(profileReads).toBe(2));
    expect(
      screen.queryByPlaceholderText("Device name"),
    ).not.toBeInTheDocument();
    await act(async () => {
      if (outcome === "success")
        finish(wire({ setProfileName: { success: true } }));
      else fail(new Error("Retired name save"));
    });
    expect(profileReads).toBe(2);
    expect(screen.queryByText(/Retired name save/)).not.toBeInTheDocument();
    expect(screen.getAllByText("No device paired in this slot.")).toHaveLength(
      5,
    );
  },
);

it("retries the same label after encoded refusal and succeeds", async () => {
  let saves = 0;
  const sent: string[] = [];
  intercept = (request) => {
    if (!request.setProfileName) return undefined;
    saves++;
    sent.push(request.setProfileName.name);
    if (saves === 1)
      return Promise.resolve(wire({ error: { message: "Retry refusal" } }));
    name = request.setProfileName.name;
    return Promise.resolve(wire({ setProfileName: { success: true } }));
  };
  render(<App />);
  await screen.findByText("fixture-host");
  fireEvent.click(screen.getByTitle("Edit name"));
  fireEvent.change(screen.getByPlaceholderText("Device name"), {
    target: { value: "retry-unchanged" },
  });
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await screen.findByText(/Retry refusal/);
  expect(screen.getByPlaceholderText("Device name")).toBeEnabled();
  expect(screen.getByPlaceholderText("Device name")).toHaveValue(
    "retry-unchanged",
  );
  fireEvent.click(screen.getByRole("button", { name: /Save/ }));
  await screen.findByText("retry-unchanged");
  expect(sent).toEqual(["retry-unchanged", "retry-unchanged"]);
  expect(profileReads).toBe(2);
});
