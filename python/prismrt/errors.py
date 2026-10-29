"""Exceptions of prismrt: one per prism::ErrorCode (include/prism/types.h).

Every exception derives from :class:`Error` (itself a ``RuntimeError``) and
from the builtin that matches its meaning, so ``except ValueError`` also
catches an :class:`InvalidArgumentError`.
"""

from __future__ import annotations


class Error(RuntimeError):
    """An error of the renderer (prism::Error)."""

    code = "Internal"


class InvalidArgumentError(Error, ValueError):
    """Misuse: an invalid value, shape, setting or reference."""

    code = "InvalidArgument"


class InvalidHandleError(Error, ValueError):
    """A handle of a removed scene object."""

    code = "InvalidHandle"


class NotFoundError(Error, LookupError):
    """A missing file, prim, AOV or stage."""

    code = "NotFound"


class UnsupportedError(Error, NotImplementedError):
    """A device or feature this build or machine lacks (e.g. allow_fallback=False)."""

    code = "Unsupported"


class DeviceLostError(Error):
    """The GPU device was lost."""

    code = "DeviceLost"


class OutOfMemoryError(Error, MemoryError):
    """The renderer ran out of memory."""

    code = "OutOfMemory"


class CancelledError(Error):
    """A progress callback returned False, or cancel() stopped the call."""

    code = "Cancelled"


class InternalError(Error):
    """An internal error of the renderer."""

    code = "Internal"


ERRORS = {cls.code: cls for cls in (InvalidArgumentError, InvalidHandleError, NotFoundError, UnsupportedError,
                                    DeviceLostError, OutOfMemoryError, CancelledError, InternalError)}

__all__ = ["Error"] + [cls.__name__ for cls in ERRORS.values()]
