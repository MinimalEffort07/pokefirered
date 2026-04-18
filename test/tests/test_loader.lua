-- Minimal loader that dofile()s the actual test
-- This works around a possible mGBA bug with large --script files
dofile("test/tests/test_gc_check.lua")
