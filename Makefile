CXX      := g++
CXXFLAGS := -Wall -Wextra -std=c++17 -Wno-missing-field-initializers -Iinclude `pkg-config fuse3 --cflags`
LDFLAGS  := `pkg-config fuse3 --libs`
TARGET   := kms

SRC_DIR  := src
OBJ_DIR  := obj

# Tüm kaynak dosyalarını topluyoruz
SRCS     := main.cpp $(wildcard $(SRC_DIR)/*.cpp)
# Nesne dosyalarını obj/ klasörü altında eşliyoruz
OBJS     := $(OBJ_DIR)/main.o $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(wildcard $(SRC_DIR)/*.cpp))

all: $(TARGET)

# Bağlama (Link) İşlemi
$(TARGET): $(OBJS)
	@echo "[LINK] $@ oluşturuluyor..."
	@$(CXX) $(OBJS) $(LDFLAGS) -o $@

# Kök dizindeki main.cpp için derleme kuralı
$(OBJ_DIR)/main.o: main.cpp | $(OBJ_DIR)
	@echo "[CXX] main.cpp -> $@"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# src/ altındaki tüm kaynak dosyalar için derleme kuralı
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	@echo "[CXX] $< -> $@"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# Nesne dosyaları için klasör oluşturma
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

clean:
	@echo "[CLEAN] Geçici dosyalar temizleniyor..."
	@rm -rf $(OBJ_DIR) $(TARGET)

.PHONY: all clean