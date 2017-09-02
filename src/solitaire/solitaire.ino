#include <SPI.h>
#include <Gamebuino.h>
#include <EEPROM.h>
#include "pile.h"

#define MAX_CARDS_DRAWN_IN_PILE 10

Gamebuino gb;

enum GameMode { dealing, selecting, drawingCards, movingPile, illegalMove, wonGame };
// State of the game.
GameMode mode = selecting;

// Stock: where you draw cards from
// Talon: drawn cards
// Foundation: get the cards here in suit order to win
// Tableau: alternating colors, descending order
enum Location { stock, talon, 
  foundation1, foundation2, foundation3, foundation4,
  tableau1, tableau2, tableau3, tableau4, tableau5, tableau6, tableau7 }; 
// Stack that the cursor is currently pointed at.
Location activeLocation;
// Within the stack, card position for the cursor, 0 being top card.
byte cardIndex;
// Position of the cursor for animation.
byte cursorX, cursorY;

// Animating moving stack of cards.
Pile moving = Pile(13);
Location returnLocation;
byte remainingDraws;
// 3 at a time for hard, 1 at a time for easy
byte cardsToDraw;

// Keep track of source pile for returning invalid moves.
Pile *sourcePile;

Pile stockDeck = Pile(52), talonDeck = Pile(52);
Pile foundations[4] = { Pile(13), Pile(13), Pile(13), Pile(13) };
Pile tableau[7] = { Pile(20), Pile(20), Pile(20), Pile(20), Pile(20), Pile(20), Pile(20) };

struct CardAnimation {
  Card card;
  byte tableauIndex, x, y, destX, destY;
};

// Used to deal at the start of the game.
CardAnimation cardAnimations[28];
byte cardAnimationCount = 0;

struct CardBounce {
  Card card;
  int x, y, xVelocity, yVelocity;
};

// Keeps track of a bouncing card for the winning animation.
CardBounce bounce;
byte bounceIndex;

const char easy[] PROGMEM = "Easy";
const char hard[] PROGMEM = "Hard";
const char* const menu[2] PROGMEM = {
  easy,
  hard,
};

void setup() {
  gb.begin();

  // Initialize positions of piles.
  for (int i = 0; i < 4; i++) {
    foundations[i].x = 37 + i * 12;
    foundations[i].y = 1;
    foundations[i].isTableau = false;
  }
  for (int i = 0; i < 7; i++) {
    tableau[i].x = i * 12 + 1;
    tableau[i].y = 17;
    tableau[i].isTableau = true;
  }
  stockDeck.x = 1;
  stockDeck.y = 1;
  stockDeck.isTableau = false;
  talonDeck.x = 13;
  talonDeck.y = 1;
  stockDeck.isTableau = false;
  
  showTitle();
}

void loop() {
  // Main loop.
  if (gb.update()) {
    // Exit to title whenever C is pressed.
    if (gb.buttons.pressed(BTN_C)) showTitle();
    
    // Handle key presses for various modes.
    switch (mode) {
      case selecting: handleSelectingButtons(); break;
      case movingPile: handleMovingPileButtons(); break;
    }
    
    // Draw the board.
    if (mode != wonGame) {
      drawBoard();      
    }
    
    // Draw other things based on the current state of the game.
    switch(mode) {
      case dealing: drawDealing(); break;
      case selecting: drawCursor(); break;
      case drawingCards: drawDrawingCards(); break;
      case movingPile: drawMovingPile(); break;
      case illegalMove: drawIllegalMove(); break;
      case wonGame: drawWonGame(); break;
    }
  }
}

void showTitle() {
  gb.display.persistence = true;
  gb.titleScreen(F("Solitaire"));
  gb.pickRandomSeed();
  gb.battery.show = false;
  setupNewGame();

  // Ask whether we want easy (flip 1 card per draw) or hard (flip 3 cards per draw).
  byte menuOption;
  do {
    menuOption = gb.menu(menu, 2);
  } while (menuOption == -1);
  cardsToDraw = menuOption == 0 ? 1 : 3;
}

void setupNewGame() {
  activeLocation = stock;
  cardIndex = 0;
  cursorX = 11;
  cursorY = 5;

  talonDeck.empty();
  stockDeck.newDeck();
  stockDeck.shuffle();
  for (int i = 0; i < 4; i++) {
    foundations[i].empty();
  }
  for (int i = 0; i < 7; i++) {
    tableau[i].empty();
  }

  // Initialize the data structure to deal out the initial board.
  cardAnimationCount = 0;
  for (int i = 0; i < 7; i++) {
    for (int j = i; j < 7; j++) {
      Card card = stockDeck.removeTopCard();
      if (i == j) card.flip();
      cardAnimations[cardAnimationCount] = CardAnimation();
      cardAnimations[cardAnimationCount].x = 1;
      cardAnimations[cardAnimationCount].y = 1;
      cardAnimations[cardAnimationCount].destX = tableau[j].x;
      cardAnimations[cardAnimationCount].destY = tableau[j].y + 2 * i;
      cardAnimations[cardAnimationCount].tableauIndex = j;
      cardAnimations[cardAnimationCount].card = card;
      cardAnimationCount++;
    }
  }
  cardAnimationCount = 0;

  mode = dealing; 
}

void handleSelectingButtons() {
  // Handle buttons when user is using the arrow cursor to navigate.
  Location originalLocation = activeLocation;
  if (gb.buttons.pressed(BTN_RIGHT)) {
    if (activeLocation != foundation4 && activeLocation != tableau7) {
      activeLocation = activeLocation + 1;
    }
  }
  if (gb.buttons.pressed(BTN_LEFT)) {
    if (activeLocation != stock && activeLocation != tableau1) {
      activeLocation = activeLocation - 1;
    }
  }
  if (gb.buttons.pressed(BTN_DOWN)) {
    if (cardIndex > 0) {
      cardIndex--;
    }
    else {
      if (activeLocation < foundation1) activeLocation = activeLocation + 6;
      else if (activeLocation <= foundation4) activeLocation = activeLocation + 7;
    }
  }
  if (gb.buttons.pressed(BTN_UP)) {
    bool interPileNavigation = false;
    if (activeLocation >= tableau1 && activeLocation <= tableau7) {
      Pile *pile = getActiveLocationPile();
      if (pile->getCardCount() > cardIndex + 1 && !pile->getCard(cardIndex + 1).isFaceDown()) {
        cardIndex++;
        interPileNavigation = true;
      }
    }
    if (!interPileNavigation) {
      if (activeLocation > tableau2) activeLocation = activeLocation - 7;
      else if (activeLocation >= tableau1) activeLocation = activeLocation - 6;
    }
  }
  if (gb.buttons.pressed(BTN_B)) {
    if (activeLocation >= tableau1 || activeLocation == talon) {
      Pile *pile = getActiveLocationPile();
      if (pile->getCardCount() > 0) {
        Card card = pile->getCard(0);
        bool foundMatch = false;
        for (int i = 0; i < 4; i++) {
          if (foundations[i].getCardCount() == 0 && card.getValue() == ace) {
            foundMatch = true;
          }
          else {
            Card card1 = foundations[i].getCard(0);
            Card card2 = pile->getCard(0);
            if (card1.getSuit() == card2.getSuit() && card1.getValue() + 1 == card2.getValue()) {
              foundMatch = true;
            }
          }
          if (foundMatch) {
            moving.empty();
            moving.x = pile->x;
            moving.y = cardYPosition(pile, 0);
            moving.addCard(pile->removeTopCard());
            sourcePile = &foundations[i];
            mode = illegalMove;
            break;
          }
        }
      }
    }
  }
  if (gb.buttons.pressed(BTN_A)) {
    switch (activeLocation) {
      case stock:
        if (stockDeck.getCardCount() != 0) {
          moving.empty();
          Card card = stockDeck.removeTopCard();
          card.flip();
          moving.addCard(card);
          moving.x = 1;
          moving.y = 1;
          remainingDraws = min(cardsToDraw - 1, stockDeck.getCardCount()); 
          mode = drawingCards;
        }
        else {
          while (talonDeck.getCardCount() != 0) {
            Card card = talonDeck.removeTopCard();
            card.flip();
            stockDeck.addCard(card);
          }
        }
        break;
      case talon:
      case tableau1:
      case tableau2:
      case tableau3:
      case tableau4:
      case tableau5:
      case tableau6:
      case tableau7:
        sourcePile = getActiveLocationPile();
        if (sourcePile->getCardCount() == 0) break;
        moving.empty();
        moving.x = sourcePile->x;
        moving.y = cardYPosition(sourcePile, 0);
        sourcePile->removeCards(cardIndex + 1, &moving);
        mode = movingPile;
        break;
    }
  }
  if (originalLocation != activeLocation) cardIndex = 0;
}

void handleMovingPileButtons() {
  // Handle buttons when user is moving a pile of cards.
  if (gb.buttons.pressed(BTN_RIGHT)) {
    if (activeLocation != foundation4 && activeLocation != tableau7) {
      activeLocation = activeLocation + 1;
    }
  }
  if (gb.buttons.pressed(BTN_LEFT)) {
    if (activeLocation != talon && activeLocation != tableau1) {
      activeLocation = activeLocation - 1;
    }
  }
  if (gb.buttons.pressed(BTN_DOWN)) {
    if (activeLocation == talon) activeLocation = tableau2;
    else if (activeLocation <= foundation4) activeLocation = activeLocation + 7;
  }
  if (gb.buttons.pressed(BTN_UP)) {
    if (activeLocation >= tableau4) activeLocation = activeLocation - 7;
    else if (activeLocation >= tableau1) activeLocation = talon;
  }
  if (gb.buttons.pressed(BTN_A)) {
    switch (activeLocation) {
      case talon:
        mode = illegalMove;
        break;
      case foundation1:
      case foundation2:
      case foundation3:
      case foundation4:
        {
          if (moving.getCardCount() != 1) {
            mode = illegalMove;
            break;
          }
          Pile *destinationFoundation = getActiveLocationPile();
          if (destinationFoundation->getCardCount() == 0) {
            if (moving.getCard(0).getValue() != ace) {
              mode = illegalMove;
              break;
            }
          }
          else {
            Card card1 = destinationFoundation->getCard(0);
            Card card2 = moving.getCard(0);
            if (card1.getSuit() != card2.getSuit() || card1.getValue() + 1 != card2.getValue()) {
              mode = illegalMove;
              break;
            }
          }
          moveCards();
          checkWonGame();
        }
        break;
      case tableau1:
      case tableau2:
      case tableau3:
      case tableau4:
      case tableau5:
      case tableau6:
      case tableau7:
        {
          Pile *destinationTableau = getActiveLocationPile();
          if (destinationTableau->getCardCount() > 0) {
            // Make sure that it is a decending value, alternating color.
            Card card1 = destinationTableau->getCard(0);
            Card card2 = moving.getCard(moving.getCardCount() - 1);
            if (card1.isRed() == card2.isRed() || card1.getValue() != card2.getValue() + 1) {
              mode = illegalMove;
              break;
            }
          }
          else {
            // You can only place kings in an empty tableau.
            Card card = moving.getCard(moving.getCardCount() - 1);
            if (card.getValue() != king) {
              mode = illegalMove;
              break;
            }
          }
        }
        moveCards();
        break;
    }
  }
}

void moveCards() {
  getActiveLocationPile()->addPile(&moving);
  mode = selecting;
  updateAfterPlay();
}

void updateAfterPlay() {
  revealCards();
  checkWonGame();
  cardIndex = 0;
}

void revealCards() {
  // Check for cards to reveal.
  for (int i = 0; i < 7; i++) {
    if (tableau[i].getCardCount() == 0) continue;
    Card card = tableau[i].removeTopCard();
    if (card.isFaceDown()) card.flip();
    tableau[i].addCard(card);
  }
}

void checkWonGame() {
  // Check to see if all foundations are full
  if (foundations[0].getCardCount() == 13 && foundations[1].getCardCount() == 13 &&
    foundations[2].getCardCount() == 13 && foundations[3].getCardCount() == 13) {
      mode = wonGame;
  }
}

void drawBoard() {
  // Stock
  if (stockDeck.getCardCount() != 0) {
    drawCard(stockDeck.x, stockDeck.y, Card(ace, spade, true));
  }
  
  // Talon
  for (int i = 0; i < min(3, talonDeck.getCardCount()); i++) {
    drawCard(talonDeck.x + i * 2, talonDeck.y, talonDeck.getCard(min(3, talonDeck.getCardCount()) - i - 1));
  }
  
  // Foundations
  for (int i = 0; i < 4; i++) {
    if (foundations[i].getCardCount() != 0) {
      drawCard(foundations[i].x, foundations[i].y, foundations[i].getCard(0));
    }
    else {
      gb.display.setColor(GRAY);
      gb.display.drawRect(foundations[i].x, foundations[i].y, 10, 14);
    }
  }
  
  // Tableau
  for (int i = 0; i < 7; i++) {
    drawPile(&tableau[i]);
  }
}

void drawPile(Pile* pile) {
  int baseIndex = max(0, pile->getCardCount() - MAX_CARDS_DRAWN_IN_PILE);
  for (int i = 0; i < min(pile->getCardCount(), MAX_CARDS_DRAWN_IN_PILE); i++) {
    drawCard(pile->x, pile->y + 2 * i, pile->getCard(pile->getCardCount() - i - 1 - baseIndex));
  }
}

byte cardYPosition(Pile *pile, byte cardIndex) {
  if (pile->isTableau) {
    if (cardIndex > MAX_CARDS_DRAWN_IN_PILE - 1) return pile->y;
    return pile->y + 2 * (min(pile->getCardCount(), MAX_CARDS_DRAWN_IN_PILE) - cardIndex - 1);
  }

  return pile->y;
}

void drawCard(byte x, byte y, Card card) {
  // Fill 
  byte fill = WHITE;
  if (card.isFaceDown()) fill = GRAY;
  gb.display.setColor(fill);
  gb.display.fillRect(x + 1, y + 1, 8, 12);
  
  // Draw border
  gb.display.setColor(BLACK);
  gb.display.drawFastHLine(x + 1, y, 8);
  gb.display.drawFastHLine(x + 1, y + 13, 8);
  gb.display.drawFastVLine(x, y + 1, 12);
  gb.display.drawFastVLine(x + 9, y + 1, 12);

  if (card.isFaceDown()) return;

  if (card.isRed()) gb.display.setColor(GRAY);
  drawSuit(x + 2, y + 2, card.getSuit());
  drawValue(x + 5, y + 7, card.getValue());
}

void drawSuit(byte x, byte y, Suit suit) {
  switch (suit) {
    case spade: drawSpade(x, y); break;
    case club: drawClub(x, y); break;
    case heart: drawHeart(x, y); break;
    case diamond: drawDiamond(x, y); break;
  }
}

void drawHeart(byte x, byte y) {
  gb.display.drawPixel(x + 1, y);
  gb.display.drawPixel(x + 3, y);
  gb.display.drawFastHLine(x, y + 1, 5);
  gb.display.drawFastHLine(x, y + 2, 5);
  gb.display.drawFastHLine(x + 1, y + 3, 3);
  gb.display.drawPixel(x + 2, y + 4);
}

void drawDiamond(byte x, byte y) {
  gb.display.drawPixel(x + 2, y);
  gb.display.drawFastHLine(x + 1, y + 1, 3);
  gb.display.drawFastHLine(x, y + 2, 5);
  gb.display.drawFastHLine(x + 1, y + 3, 3);
  gb.display.drawPixel(x + 2, y + 4);
}

void drawSpade(byte x, byte y) {
  gb.display.drawPixel(x + 2, y);
  gb.display.drawFastHLine(x + 1, y + 1, 3);
  gb.display.drawFastHLine(x, y + 2, 5);
  gb.display.drawFastHLine(x, y + 3, 5);
  gb.display.drawPixel(x + 2, y + 4);
}

void drawClub(byte x, byte y) {
  gb.display.drawFastHLine(x + 1, y, 3);
  gb.display.drawFastHLine(x + 1, y + 2, 3);
  gb.display.drawFastVLine(x, y + 1, 3);
  gb.display.drawFastVLine(x + 4, y + 1, 3);
  gb.display.drawFastVLine(x + 2, y + 1, 4);
}

void drawValue(byte x, byte y, Value value) {
  switch (value) {
    case ace: drawAce(x, y); break;
    case two: drawTwo(x, y); break;
    case three: drawThree(x, y); break;
    case four: drawFour(x, y); break;
    case five: drawFive(x, y); break;
    case six: drawSix(x, y); break;
    case seven: drawSeven(x, y); break;
    case eight: drawEight(x, y); break;
    case nine: drawNine(x, y); break;
    case ten: drawTen(x, y); break;
    case jack: drawJack(x, y); break;
    case queen: drawQueen(x, y); break;
    case king: drawKing(x, y); break;
  }
}

void drawAce(byte x, byte y) {
  gb.display.drawPixel(x + 1, y);
  gb.display.drawFastVLine(x, y + 1, 4);
  gb.display.drawFastVLine(x + 2, y + 1, 4);
  gb.display.drawPixel(x + 1, y + 2);
}

void drawTwo(byte x, byte y) {
  drawSegmentA(x, y);
  drawSegmentB(x, y);
  drawSegmentG(x, y);
  drawSegmentE(x, y);
  drawSegmentD(x, y);
}

void drawThree(byte x, byte y) {
  drawSegmentA(x, y);
  drawSegmentB(x, y);
  drawSegmentG(x, y);
  drawSegmentC(x, y);
  drawSegmentD(x, y);
}

void drawFour(byte x, byte y) {
  drawSegmentF(x, y);
  drawSegmentG(x, y);
  drawSegmentB(x, y);
  drawSegmentC(x, y);
}

void drawFive(byte x, byte y) {
  drawSegmentA(x, y);
  drawSegmentF(x, y);
  drawSegmentG(x, y);
  drawSegmentC(x, y);
  drawSegmentD(x, y);
}

void drawSix(byte x, byte y) {
  drawFive(x, y);
  drawSegmentE(x, y);
}

void drawSeven(byte x, byte y) {
  drawSegmentA(x, y);
  drawSegmentB(x, y);
  drawSegmentC(x, y);
}

void drawEight(byte x, byte y) {
  drawSix(x, y);
  drawSegmentB(x, y);
}

void drawNine(byte x, byte y) {
  drawFour(x, y);
  drawSegmentA(x, y);
}

void drawTen(byte x, byte y) {
  drawSeven(x, y);
  drawSegmentD(x, y);
  drawSegmentE(x, y);
  drawSegmentF(x, y);
  gb.display.drawFastVLine(x - 2, y, 5);
}

void drawJack(byte x, byte y) {
  drawSegmentB(x, y);
  drawSegmentC(x, y);
  drawSegmentD(x, y);
  gb.display.drawPixel(x, y + 3);
}

void drawQueen(byte x, byte y) {
  drawSegmentA(x, y);
  drawSegmentB(x, y);
  drawSegmentF(x, y);
  gb.display.drawFastHLine(x, y + 3, 3);
  gb.display.drawPixel(x + 1, y + 4);
}

void drawKing(byte x, byte y) {
  drawSegmentF(x, y);
  drawSegmentE(x, y);
  gb.display.drawPixel(x + 1, y + 2);
  gb.display.drawFastVLine(x + 2, y, 2);
  gb.display.drawFastVLine(x + 2, y + 3, 2);
}

void drawCursor() {
  bool flipped = false;
  switch (activeLocation) {
    case stock:
      cursorX = updatePosition(cursorX, 11);
      cursorY = updatePosition(cursorY, 5);
      break;
    case talon:
      cursorX = updatePosition(cursorX, 23 + 2 * min(2, max(0, talonDeck.getCardCount() - 1)));
      cursorY = updatePosition(cursorY, 5);
      break;
    case foundation1:
      cursorX = updatePosition(cursorX, 30);
      cursorY = updatePosition(cursorY, 5);
      flipped = true;
      break;
    case foundation2:
      cursorX = updatePosition(cursorX, 42);
      cursorY = updatePosition(cursorY, 5);
      flipped = true;
      break;
    case foundation3:
      cursorX = updatePosition(cursorX, 54);
      cursorY = updatePosition(cursorY, 5);
      flipped = true;
      break;
    case foundation4:
      cursorX = updatePosition(cursorX, 66);
      cursorY = updatePosition(cursorY, 5);
      flipped = true;
      break;
    case tableau1:
    case tableau2:
    case tableau3:
    case tableau4:
    case tableau5:
    case tableau6:
    case tableau7:
      if (activeLocation <= tableau3) {
        cursorX = updatePosition(cursorX, tableau[activeLocation - tableau1].x + 10);
      }
      else {
        cursorX = updatePosition(cursorX, tableau[activeLocation - tableau1].x - 7);
        flipped = true;
      }
      if (cardIndex == 0) {
        cursorY = updatePosition(cursorY, 4 + cardYPosition(getActiveLocationPile(), cardIndex));
      }
      else {
        cursorY = updatePosition(cursorY, -2 + cardYPosition(getActiveLocationPile(), cardIndex));
      }
      break;
  }
  drawCursor(cursorX, cursorY, flipped);
}

void drawDealing() {
  if (cardAnimationCount < 28 && gb.frameCount % 4 == 0) cardAnimationCount++;
  bool doneDealing = cardAnimationCount == 28;
  for (int i = 0; i < cardAnimationCount; i++) {
    if (cardAnimations[i].x != cardAnimations[i].destX || cardAnimations[i].y != cardAnimations[i].destY) {
      doneDealing = false;
      drawCard(cardAnimations[i].x, cardAnimations[i].y, cardAnimations[i].card);
      cardAnimations[i].x = updatePosition(cardAnimations[i].x, cardAnimations[i].destX);
      cardAnimations[i].y = updatePosition(cardAnimations[i].y, cardAnimations[i].destY);
      if (cardAnimations[i].x == cardAnimations[i].destX && cardAnimations[i].y == cardAnimations[i].destY) {
        tableau[cardAnimations[i].tableauIndex].addCard(cardAnimations[i].card);
      }
    }
  }
  if (doneDealing) mode = selecting;
}

void drawDrawingCards() {
  drawPile(&moving);
  moving.x = updatePosition(moving.x, 17);
  moving.y = updatePosition(moving.y, 1);
  if (moving.x == 17 && moving.y == 1) {
    talonDeck.addCard(moving.getCard(0));
    if (remainingDraws) {
      remainingDraws--;
      moving.empty();
      Card card = stockDeck.removeTopCard();
      card.flip();
      moving.addCard(card);
      moving.x = 1;
      moving.y = 1;
    }
    else {
      mode = selecting;
    }
  }
}

void drawMovingPile() {
  drawPile(&moving);
  Pile* pile = getActiveLocationPile();
  byte yDelta = 2;
  if (pile->isTableau) yDelta += 2 * pile->getCardCount();
  moving.x = updatePosition(moving.x, pile->x);
  moving.y = updatePosition(moving.y, pile->y + yDelta);  
}

void drawIllegalMove() {
  // Move the cards back to the source pile.
  byte yDelta = 0;
  if (sourcePile->isTableau) yDelta += 2 * sourcePile->getCardCount();
  moving.x = updatePosition(moving.x, sourcePile->x);
  moving.y = updatePosition(moving.y, sourcePile->y + yDelta); 
  drawPile(&moving);
  // Check to see if the animation is done
  if (moving.x == sourcePile->x && moving.y == sourcePile->y + yDelta) {
    sourcePile->addPile(&moving);
    mode = selecting;
    updateAfterPlay();
  }
}

void drawWonGame() {
  // Bounce the cards from the foundations, one at a time.
  if (!gb.display.persistence) {
    gb.display.persistence = true;
    drawBoard();
    initializeCardBounce();
  }

  // Apply gravity
  bounce.yVelocity += 0x0080;
  bounce.x += bounce.xVelocity;
  bounce.y += bounce.yVelocity;
  // If the card is at the bottom of the screen, reverse the y velocity and scale by 80%.
  if (bounce.y + (14 << 8) > LCDHEIGHT << 8) {
    bounce.y = (LCDHEIGHT - 14) << 8;
    bounce.yVelocity = bounce.yVelocity * -4 / 5;
  }
  drawCard(bounce.x >> 8, bounce.y >> 8, bounce.card);
  // Check to see if the current card is off the screen.
  if (bounce.x + (10 << 8) < 0 || bounce.x > LCDWIDTH << 8) {
    if (!initializeCardBounce()) showTitle();
  }
}

bool initializeCardBounce() {
  // Return false if all the cards are done.
  if (foundations[bounceIndex].getCardCount() == 0) return false;
  // Pick the next card to animate, with a random initial velocity.
  bounce.card = foundations[bounceIndex].removeTopCard();
  bounce.x = foundations[bounceIndex].x << 8;
  bounce.y = foundations[bounceIndex].y << 8;
  bounce.xVelocity = (random(2) ? 1 : -1) * random(0x0100, 0x0200);
  bounce.yVelocity = -1 * random(0x0200);
  bounceIndex = (bounceIndex + 1) % 4;
  return true;
}

Pile* getActiveLocationPile() {
  switch (activeLocation) {
    case stock:
      return &stockDeck;
    case talon:
      return &talonDeck;
    case foundation1:
    case foundation2:
    case foundation3:
    case foundation4:
      return &foundations[activeLocation - foundation1];
    case tableau1:
    case tableau2:
    case tableau3:
    case tableau4:
    case tableau5:
    case tableau6:
    case tableau7:
      return &tableau[activeLocation - tableau1];
  }
}

byte updatePosition(byte current, byte destination) {
  if (current == destination) return current;

  byte delta = (destination - current) / 3;
  if (delta == 0 && ((gb.frameCount % 3) == 0)) delta = destination > current ? 1 : -1;
  return current + delta;
}

void drawCursor(byte x, byte y, bool flipped) {
  if (flipped) {
    for (int i = 0; i < 4; i++) {
      gb.display.setColor(BLACK);
      gb.display.drawPixel(x + 3 + i, y + i);
      gb.display.drawPixel(x + 3 + i, y + (6 - i));
      gb.display.setColor(WHITE);
      gb.display.drawFastHLine(x + 3, y + i, i);
      gb.display.drawFastHLine(x + 3, y + (6 - i), i);
    }
    gb.display.setColor(BLACK);
    gb.display.drawFastVLine(x + 2, y, 7);
    gb.display.drawFastHLine(x, y + 2, 2);
    gb.display.drawFastHLine(x, y + 4, 2);
    gb.display.drawPixel(x, y + 3);
    gb.display.setColor(WHITE);
    gb.display.drawFastHLine(x + 1, y + 3, 2);
    if (cardIndex != 0) {
      Card card = getActiveLocationPile()->getCard(cardIndex);
      byte extraWidth = card.getValue() == ten ? 2 : 0;
      gb.display.setColor(BLACK);
      gb.display.drawRect(x - 12 - extraWidth, y - 1, 13 + extraWidth, 9);
      gb.display.setColor(WHITE);
      gb.display.drawPixel(x, y + 3);
      gb.display.fillRect(x - 11 - extraWidth, y, 11 + extraWidth, 7);
      gb.display.setColor(card.isRed() ? GRAY : BLACK);
      drawValue(x - 10, y + 1, card.getValue());
      drawSuit(x - 6, y + 1, card.getSuit());
    }
  }
  else {
    for (int i = 0; i < 4; i++) {
      gb.display.setColor(BLACK);
      gb.display.drawPixel(x + 3 - i, y + i);
      gb.display.drawPixel(x + 3 - i, y + (6 - i));
      gb.display.setColor(WHITE);
      gb.display.drawFastHLine(x + 4 - i, y + i, i);
      gb.display.drawFastHLine(x + 4 - i, y + (6 - i), i);
    }
    gb.display.setColor(BLACK);
    gb.display.drawFastVLine(x + 4, y, 7);
    gb.display.drawFastHLine(x + 5, y + 2, 2);
    gb.display.drawFastHLine(x + 5, y + 4, 2);
    gb.display.drawPixel(x + 6, y + 3);
    gb.display.setColor(WHITE);
    gb.display.drawFastHLine(x + 4, y + 3, 2);
    if (cardIndex != 0) {
      Card card = getActiveLocationPile()->getCard(cardIndex);
      byte extraWidth = card.getValue() == ten ? 2 : 0;
      gb.display.setColor(BLACK);
      gb.display.drawRect(x + 6, y - 1, 13 + extraWidth, 9);
      gb.display.setColor(WHITE);
      gb.display.drawPixel(x + 6, y + 3);
      gb.display.fillRect(x + 7, y, 11 + extraWidth, 7);
      gb.display.setColor(card.isRed() ? GRAY : BLACK);
      drawValue(x + 8 + extraWidth, y + 1, card.getValue());
      drawSuit(x + 12 + extraWidth, y + 1, card.getSuit());
    }
  }
}

void drawSegmentA(byte x, byte y) {
  gb.display.drawFastHLine(x, y, 3);
}

void drawSegmentB(byte x, byte y) {
  gb.display.drawFastVLine(x + 2, y, 3);
}

void drawSegmentC(byte x, byte y) {
  gb.display.drawFastVLine(x + 2, y + 2, 3);
}

void drawSegmentD(byte x, byte y) {
  gb.display.drawFastHLine(x, y + 4, 3);
}

void drawSegmentE(byte x, byte y) {
  gb.display.drawFastVLine(x, y + 2, 3);
}

void drawSegmentF(byte x, byte y) {
  gb.display.drawFastVLine(x, y, 3);
}

void drawSegmentG(byte x, byte y) {
  gb.display.drawFastHLine(x, y + 2, 3);
}

