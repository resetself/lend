all: lendd
	@echo "构建完成，运行 make install 安装"

lendd:
	cd lend && go build -o ../build/lendd ./cmd/lendd

lendctl:
	gcc -o build/lendctl lend/cmd/lendctl/lendctl.c

clean:
	rm -rf build

install: lendd
	@mkdir -p $(HOME)/.lend/bin
	@cp build/lendd $(HOME)/.lend/bin/
	@./install.sh --local

.PHONY: all clean install
