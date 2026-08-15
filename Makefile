all: lendd lendctl
	@echo "构建完成，运行 make install 安装"

lendd:
	@mkdir -p build
	cd lend && go build -o ../build/lendd ./cmd/lendd

lendctl:
	@mkdir -p build
	gcc -o build/lendctl lend/cmd/lendctl/lendctl.c

clean:
	rm -rf build

install: lendd
	@mkdir -p $(HOME)/.lend/bin
	@cp build/lendd $(HOME)/.lend/bin/
	@./install.sh --local

.PHONY: all clean install lendd lendctl
