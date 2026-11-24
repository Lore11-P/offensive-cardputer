// name: Breakout
// desc: Rompi tutti i mattoni con tre vite.
// Breakout per ORBIT OS - 240x135, ENTER per giocare di nuovo.
var bg = Game.rgb(8, 10, 18)
var panel = Game.rgb(18, 22, 34)
var fg = Game.rgb(225, 238, 255)
var acc = Game.rgb(90, 220, 255)
var hot = Game.rgb(255, 190, 60)
var ok = Game.rgb(110, 240, 160)
var dim = Game.rgb(110, 125, 150)
var warn = Game.rgb(255, 90, 95)

var top = 15
var paddleY = 125
var paddleW = 36
var state = 0
var px = 102
var ballX = 118
var ballY = 119
var vx = 2.0
var vy = -2.6
var score = 0
var lives = 3
var left = 0
var wait = 0
var won = false
var bricks = []
var rowColors = [hot, acc, ok, warn]
var buildBricks = Fn.new {
  bricks = []
  left = 0
  for (r in 0...3) {
    for (c in 0...5) {
      bricks.add({"x": 2 + c * 40, "y": 24 + r * 12, "row": r, "alive": true})
      left += 1
    }
  }
}
var resetBall = Fn.new {
  px = 102
  ballX = px + 16
  ballY = paddleY - 6
  vx = 2.0
  vy = -2.6
  wait = Game.millis + 600
}

var newGame = Fn.new {
  score = 0
  lives = 3
  won = false
  buildBricks.call()
  resetBall.call()
  state = 1
}

var drawHeader = Fn.new {
  Game.rect(0, 0, Game.width, top, panel)
  Game.text(6, 3, "BREAKOUT", acc, 0)
  Game.text(108, 3, "PUNTI %(score)", fg, 0)
  var v = "VITE %(lives)"
  Game.text(Game.width - 6 - (v.count * 6 - 1), 3, v, hot, 0)
}

var drawGame = Fn.new {
  Game.rect(0, top, Game.width, Game.height - top, bg)
  for (b in bricks) {
    if (b["alive"]) {
      Game.rect(b["x"], b["y"], 36, 8, rowColors[b["row"]])
      Game.rect(b["x"] + 2, b["y"] + 2, 32, 2, panel)
    }
  }
  Game.rect(px, paddleY, paddleW, 6, acc)
  Game.rect(px + 14, paddleY + 2, 8, 2, fg)
  if (Game.millis >= wait || Game.millis % 240 < 120) {
    Game.rect(ballX, ballY, 4, 4, fg)
  }
  if (state == 2) {
    Game.rect(40, 48, 160, 46, panel)
    var title = "GAME OVER"
    var color = warn
    if (won) {
      title = "HAI VINTO!"
      color = ok
    }
    Game.text((Game.width - (title.count * 7 - 2)) / 2, 53, title, color, 2)
    var s = "PUNTI %(score)"
    Game.text((Game.width - (s.count * 7 - 2)) / 2, 67, s, fg, 2)
    Game.text(50, 81, "ENTER riparti - ESC esci", dim, 0)
  }
}

var drawTitle = Fn.new {
  Game.rect(0, 0, Game.width, Game.height, bg)
  Game.text(73, 22, "BREAKOUT", acc, 4)
  for (c in 0...6) {
    var colors = [hot, acc, ok, warn, hot, acc]
    Game.rect(2 + c * 40, 44, 36, 8, colors[c])
  }
  Game.text(54, 64, "Rompi tutti i mattoni.", fg, 0)
  Game.rect(102, 80, 36, 6, acc)
  Game.rect(118, 72, 4, 4, fg)
  Game.text(53, 96, "SINISTRA/DESTRA: muovi", hot, 0)
  Game.text(48, 109, "ENTER: gioca   ESC: esci", dim, 0)
}

newGame.call()
state = 0

while (true) {
  Game.frame()
  if (state == 0) {
    drawTitle.call()
    if (Game.select) {
      newGame.call()
    }
  } else {
    if (state == 1) {
      if (Game.left) {
        px -= 5
      }
      if (Game.right) {
        px += 5
      }
      if (px < 2) {
        px = 2
      }
      if (px > Game.width - paddleW - 2) {
        px = Game.width - paddleW - 2
      }
      if (Game.millis >= wait) {
        ballX += vx
        ballY += vy
        if (ballX < 0) {
          ballX = 0
          vx = -vx
        }
        if (ballX > Game.width - 4) {
          ballX = Game.width - 4
          vx = -vx
        }
        if (ballY < top) {
          ballY = top
          vy = -vy
        }
        if (vy > 0 && ballY + 4 >= paddleY && ballY + 4 <= paddleY + 10 &&
            ballX + 4 >= px && ballX <= px + paddleW) {
          ballY = paddleY - 4
          vy = -vy
          vx = ((ballX + 2) - (px + 18)) / 6
          if (vx > 3.2) {
            vx = 3.2
          }
          if (vx < -3.2) {
            vx = -3.2
          }
          if (vx > -1.0 && vx < 1.0) {
            if (vx < 0) {
              vx = -1.0
            } else {
              vx = 1.0
            }
          }
          Game.beep(440, 20)
        }
        for (b in bricks) {
          if (b["alive"] && ballX + 4 >= b["x"] && ballX <= b["x"] + 36 &&
              ballY + 4 >= b["y"] && ballY <= b["y"] + 8) {
            b["alive"] = false
            left -= 1
            score += 10
            vy = -vy
            Game.beep(880, 15)
            break
          }
        }
        if (left <= 0) {
          won = true
          state = 2
          Game.beep(1100, 180)
        }
        if (ballY > Game.height) {
          lives -= 1
          Game.beep(140, 250)
          if (lives <= 0) {
            won = false
            state = 2
          } else {
            resetBall.call()
          }
        }
      }
    } else if (Game.select) {
      newGame.call()
    }
    drawHeader.call()
    drawGame.call()
  }
  Game.present()
}
