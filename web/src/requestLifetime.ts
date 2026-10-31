import { useCallback, useEffect, useRef, useState } from "react";

// Reset the panel's complete state when its connection or subsystem changes.
export function useConnectionKey(
  connection: unknown,
  index: number | undefined
) {
  const [source, setSource] = useState({ connection, index, key: 0 });
  if (source.connection !== connection || source.index !== index) {
    const next = { connection, index, key: source.key + 1 };
    setSource(next);
    return next.key;
  }
  return source.key;
}

// A newer operation or unmount retires every result/error/completion callback.
export function useRequestLifetime() {
  const lifetime = useRef({ active: true, generation: 0 });
  useEffect(() => {
    const current = lifetime.current;
    current.active = true;
    return () => {
      current.active = false;
      ++current.generation;
    };
  }, []);
  return useCallback(() => {
    const current = lifetime.current;
    const generation = ++current.generation;
    return () => current.active && generation === current.generation;
  }, []);
}
