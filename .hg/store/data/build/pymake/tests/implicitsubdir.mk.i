         k   j      f���������0�����N��ߠ�E�
R�=�            u$(shell \
mkdir foo; \
touch test.in \
)

all: foo/test.out
	@echo TEST-PASS

foo/%.out: %.in
	cp $< $@


