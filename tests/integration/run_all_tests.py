#!/usr/bin/env python3
"""
MiniDrive Integration Test Runner

Runs all integration test suites and provides a summary.

Usage:
    python3 tests/integration/run_all_tests.py [--verbose] [--stop-on-failure]
"""

import subprocess
import sys
import os
import time
import argparse
from datetime import datetime

from test_utils import SERVER_EXE, CLIENT_EXE

# Test suites in execution order: (id, name, script)
TEST_SUITES = [
    ("basic", "Basic Operations", "test_basic_operations.py"),
    ("core", "Core Commands", "test_core_commands.py"),
    ("folder", "Folder Commands", "test_folder_commands.py"),
    ("auth", "Authentication", "test_authentication.py"),
    ("sync", "Sync Command", "test_sync_command.py"),
    ("batch", "Batch & Two-Way Sync", "test_batch_and_twoway_sync.py"),
    ("tiers", "Storage Tiering", "test_storage_tiers.py"),
    ("conc", "Concurrency", "test_concurrency.py"),
    ("sec", "Security", "test_security.py"),
    ("err", "Error Handling", "test_error_handling.py"),
    ("resume", "Resume Transfers", "test_resume.py"),
    ("log", "Client Logging", "test_logging.py"),
    ("multi", "Multiple Sessions", "test_multiple_sessions.py"),
]


def run_test_suite(name, script, verbose=False):
    """Run a single test suite and return (passed, output)."""
    script_path = os.path.join(os.path.dirname(__file__), script)
    
    if not os.path.exists(script_path):
        return None, f"Script not found: {script}"
    
    try:
        process = subprocess.Popen(
            [sys.executable, script_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )
        
        try:
            stdout, stderr = process.communicate(timeout=300)
        except subprocess.TimeoutExpired:
            process.terminate() # Send SIGTERM
            try:
                stdout, stderr = process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill() # Send SIGKILL
                stdout, stderr = process.communicate()
            return False, f"TIMEOUT: Test suite exceeded 5 minute limit\nOutput:\n{stdout}\n{stderr}"
        
        output = stdout + stderr
        passed = process.returncode == 0
        
        return passed, output
        
    except Exception as e:
        return False, f"ERROR: {str(e)}"


def extract_results(output):
    """Extract pass/fail counts from test output."""
    for line in output.split('\n'):
        if 'Results:' in line and '/' in line:
            # Parse "Results: X/Y passed"
            try:
                parts = line.split(':')[1].strip()
                fraction = parts.split()[0]
                passed, total = fraction.split('/')
                return int(passed), int(total)
            except:
                pass
    return None, None


def main():
    parser = argparse.ArgumentParser(description='Run all MiniDrive integration tests')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Show full output from each test suite')
    parser.add_argument('--stop-on-failure', '-x', action='store_true',
                        help='Stop running tests after first failure')
    parser.add_argument('--suite', '-s', type=str,
                        help='Run only the specified suite (partial match)')
    parser.add_argument('--list', '-l', action='store_true',
                        help='List all available test suites and exit')
    args = parser.parse_args()

    # List suites if requested
    if args.list:
        print("Available test suites:")
        print()
        print(f"  {'ID':<8} {'Name':<25} {'Script'}")
        print(f"  {'-'*6}   {'-'*23}   {'-'*30}")
        for suite_id, name, script in TEST_SUITES:
            print(f"  {suite_id:<8} {name:<25} ({script})")
        print()
        print("Use --suite <id> to run a specific suite")
        print("Examples: --suite auth, --suite sync, --suite err")
        sys.exit(0)

    # Change to project root
    project_root = os.path.dirname(os.path.dirname(os.path.dirname(__file__)))
    os.chdir(project_root)

    print("=" * 70)
    print("MiniDrive Integration Test Runner")
    print("=" * 70)
    print(f"Started: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Working directory: {os.getcwd()}")
    print()

    # Check build exists
    if not os.path.exists(SERVER_EXE) or not os.path.exists(CLIENT_EXE):
        print("ERROR: Executables not found. Run 'cmake --build build' first.")
        sys.exit(1)

    # Filter suites if --suite specified
    suites_to_run = TEST_SUITES
    if args.suite:
        suites_to_run = [(sid, name, script) for sid, name, script in TEST_SUITES 
                         if args.suite.lower() == sid.lower() or 
                            args.suite.lower() in name.lower()]
        if not suites_to_run:
            print(f"ERROR: No suite matching '{args.suite}' found.")
            print("Run with --list to see available suites.")
            sys.exit(1)

    results = []
    total_passed = 0
    total_tests = 0
    failed_suites = []

    start_time = time.time()

    for suite_id, name, script in suites_to_run:
        print(f"\n{'─' * 70}")
        print(f"Running: {name}")
        print(f"{'─' * 70}")
        
        suite_start = time.time()
        passed, output = run_test_suite(name, script, args.verbose)
        suite_duration = time.time() - suite_start
        
        if passed is None:
            print(f"  ⚠ SKIPPED: {output}")
            results.append((name, "SKIPPED", 0, 0, suite_duration))
            continue
        
        # Extract individual test counts
        suite_passed, suite_total = extract_results(output)
        
        if args.verbose:
            print(output)
        else:
            # Show just the summary lines
            for line in output.split('\n'):
                if '✓' in line or '✗' in line:
                    print(line)
                elif 'Results:' in line:
                    print(line)
        
        if passed:
            status = "PASSED"
            print(f"\n  ✓ {name}: PASSED", end="")
        else:
            status = "FAILED"
            failed_suites.append(name)
            print(f"\n  ✗ {name}: FAILED", end="")
        
        if suite_passed is not None:
            print(f" ({suite_passed}/{suite_total} tests)")
            total_passed += suite_passed
            total_tests += suite_total
        else:
            print()
        
        print(f"    Duration: {suite_duration:.1f}s")
        
        results.append((name, status, suite_passed or 0, suite_total or 0, suite_duration))
        
        if not passed and args.stop_on_failure:
            print("\n⚠ Stopping on first failure (--stop-on-failure)")
            break

    total_duration = time.time() - start_time

    # Summary
    print("\n")
    print("=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print()
    print(f"{'Suite':<25} {'Status':<10} {'Tests':<15} {'Duration':<10}")
    print("-" * 60)
    
    for name, status, passed, total, duration in results:
        if status == "SKIPPED":
            tests_str = "-"
        else:
            tests_str = f"{passed}/{total}"
        
        status_icon = "✓" if status == "PASSED" else ("⚠" if status == "SKIPPED" else "✗")
        print(f"{name:<25} {status_icon} {status:<8} {tests_str:<15} {duration:.1f}s")
    
    print("-" * 60)
    print(f"{'TOTAL':<25} {'':<10} {total_passed}/{total_tests:<13} {total_duration:.1f}s")
    print()

    suites_passed = sum(1 for _, status, _, _, _ in results if status == "PASSED")
    suites_total = sum(1 for _, status, _, _, _ in results if status != "SKIPPED")

    if failed_suites:
        print(f"❌ {len(failed_suites)} suite(s) failed: {', '.join(failed_suites)}")
        print()
        sys.exit(1)
    else:
        print(f"✅ All {suites_passed} suite(s) passed! ({total_passed} tests)")
        print()
        sys.exit(0)


if __name__ == "__main__":
    main()
