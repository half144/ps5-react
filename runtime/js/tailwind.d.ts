// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Editor types for build-time styling (docs/TAILWIND.md); the build removes className.
import 'embedded-react';

interface ClassNameProps {
  /** Tailwind-style classes compiled to `style` at build time. */
  className?: string;
  /** Read by `focused:` (and its `hover:`/`focus:` aliases). */
  focused?: boolean;
  /** Read by `selected:`. */
  selected?: boolean;
  /** Read by `active:`. */
  active?: boolean;
  /** Read by `checked:`. */
  checked?: boolean;
  /** Read by `pressed:`. */
  pressed?: boolean;
  /** Read by `disabled:`. */
  disabled?: boolean;
}

declare module 'embedded-react' {
  interface ViewProps extends ClassNameProps {}
  interface TextProps extends ClassNameProps {}
  interface ImageProps extends ClassNameProps {}
}
