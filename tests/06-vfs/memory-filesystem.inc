<?php

declare(strict_types=1);

/**
 * Array-backed FileSystem implementations, for the test suite only.
 *
 * These live under tests/ rather than stubs/ deliberately: the stubs directory
 * ships as the published stubs package and must stay interface-only. Nothing
 * here is part of the public API, and a host writing a real backend should read
 * it as a worked example rather than as something to extend.
 *
 * Two of them, because the extension takes a genuinely different path for each:
 * a plain FileSystem is buffered whole-file, a RangedFileSystem is streamed. A
 * test that exercised only one would leave half the read/write code unrun.
 *
 * Both can be told to fail on demand, which is the point of the third one: the
 * split between "VfsError, which a script may catch" and "any other exception,
 * which is the host failing and must reach the caller intact" is a security
 * boundary, and it is only testable if a backend can be made to do both.
 */

use DevelopGravity\LuaExt\Exception\VfsError;
use DevelopGravity\LuaExt\FileStat;
use DevelopGravity\LuaExt\FileSystem;
use DevelopGravity\LuaExt\RangedFileSystem;

trait MemoryFileSystemBody
{
	/** @var array<string, string> */
	public array $files = [];

	/** @var list<string> Method names that should throw a VfsError. */
	public array $refuseOn = [];

	/** @var list<string> Method names that should throw a host failure. */
	public array $failOn = [];

	/** @var list<string> Every call the extension made, in order. */
	public array $calls = [];

	/**
	 * @param array<string, string> $files
	 */
	public function __construct(array $files = [])
	{
		$this->files = $files;
	}

	/**
	 * Record the call and honour any injected failure.
	 *
	 * Called first in every method below, so a refusal happens before the
	 * operation rather than after a partial one.
	 */
	private function enter(string $method, string $path): void
	{
		$this->calls[] = sprintf('%s(%s)', $method, $path);

		if (in_array($method, $this->refuseOn, true)) {
			throw new VfsError(sprintf('%s refused for %s', $method, $path));
		}

		if (in_array($method, $this->failOn, true)) {
			// Not a VfsError: this stands for the backend itself being broken —
			// a database down, a bug — which must not become a script-visible nil.
			throw new RuntimeException(sprintf('backend exploded during %s', $method));
		}
	}

	public function exists(string $path): bool
	{
		$this->enter('exists', $path);

		return array_key_exists($path, $this->files);
	}

	public function stat(string $path): ?FileStat
	{
		$this->enter('stat', $path);

		if (!array_key_exists($path, $this->files)) {
			return null;
		}

		return new FileStat(strlen($this->files[$path]), 0);
	}

	public function read(string $path): string
	{
		$this->enter('read', $path);

		if (!array_key_exists($path, $this->files)) {
			throw new VfsError(sprintf('no such file: %s', $path));
		}

		return $this->files[$path];
	}

	public function write(string $path, string $contents): void
	{
		$this->enter('write', $path);

		$this->files[$path] = $contents;
	}

	public function delete(string $path): void
	{
		$this->enter('delete', $path);

		if (!array_key_exists($path, $this->files)) {
			throw new VfsError(sprintf('no such file: %s', $path));
		}

		unset($this->files[$path]);
	}

	public function rename(string $from, string $to): void
	{
		$this->enter('rename', $from);

		if (!array_key_exists($from, $this->files)) {
			throw new VfsError(sprintf('no such file: %s', $from));
		}

		$this->files[$to] = $this->files[$from];
		unset($this->files[$from]);
	}

	/**
	 * @return list<string>
	 */
	public function list(string $path): array
	{
		$this->enter('list', $path);

		$prefix = rtrim($path, '/') . '/';
		$names = [];

		foreach (array_keys($this->files) as $candidate) {
			if (str_starts_with($candidate, $prefix)) {
				$rest = substr($candidate, strlen($prefix));

				// Direct children only, the way a directory listing works.
				if (!str_contains($rest, '/')) {
					$names[] = $rest;
				}
			}
		}

		sort($names);

		return $names;
	}
}

/** Whole-file backend: the extension buffers, because it cannot seek. */
final class MemoryFileSystem implements FileSystem
{
	use MemoryFileSystemBody;
}

/** Seekable backend: the extension streams through the three range methods. */
final class RangedMemoryFileSystem implements RangedFileSystem
{
	use MemoryFileSystemBody;

	public function readRange(string $path, int $offset, int $length): string
	{
		$this->enter('readRange', $path);

		if (!array_key_exists($path, $this->files)) {
			throw new VfsError(sprintf('no such file: %s', $path));
		}

		return substr($this->files[$path], $offset, $length);
	}

	public function writeRange(string $path, int $offset, string $data): void
	{
		$this->enter('writeRange', $path);

		$existing = $this->files[$path] ?? '';

		// A write past the end zero-fills the gap, as a real sparse file does.
		if ($offset > strlen($existing)) {
			$existing .= str_repeat("\0", $offset - strlen($existing));
		}

		$this->files[$path] = substr_replace($existing, $data, $offset, strlen($data));
	}

	public function truncate(string $path, int $size): void
	{
		$this->enter('truncate', $path);

		$existing = $this->files[$path] ?? '';

		$this->files[$path] = $size <= strlen($existing)
			? substr($existing, 0, $size)
			: $existing . str_repeat("\0", $size - strlen($existing));
	}
}
